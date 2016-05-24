//
//  ChatBot.c
//  chatbot
//
//  Created on 5/5/16.
//  Copyright © 2016 NobodyNada. All rights reserved.
//

#include "ChatBot.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "ChatMessage.h"
#include "cJSON.h"
#include <zlib.h>
#include <ctype.h>

#define REPORT_HEADER "Potentially bad question"
#define API_KEY "HNA2dbrFtyTZxeHN6rThNg(("
#define THRESHOLD 1000

static void loadNullReports(Report **reports) {
    for (int i = 0; i < REPORT_MEMORY; i++) {
        reports[i] = NULL;
    }
}

typedef struct {
    char *text;
    unsigned trueOccurences;
    unsigned falseOccurences;
}Word;

void analyzeReports(ChatBot *bot) {
    puts("Analyzing reports...");
    Report **reports = bot->latestReports;
    //First, string together all of the true and false positive posts.
    size_t trueLen = 0;
    size_t falseLen = 0;
    char *truePositives = NULL;
    char *falsePositives = NULL;
    for (int i = 0; i < REPORT_MEMORY; i++) {
        Report *report = reports[i];
        if (!report || report->confirmation == -1) {
            continue;
        }
        const char *body = report->post->body;
        if (report->confirmation) {
            trueLen += strlen(body) + 1;
            truePositives = realloc(truePositives, trueLen + 1);
            strcat(truePositives, " ");
            strcat(truePositives, body);
        }
        else {
            falseLen += strlen(body) + 1;
            falsePositives = realloc(falsePositives, falseLen + 1);
            strcat(falsePositives, " ");
            strcat(falsePositives, body);
        }
    }
    
    //Now, count word frequency.
    Word *words = NULL;
    size_t wordCount = 0;
    unsigned char inWord = 0;
    
    const unsigned maxWordLength = 256;
    char currentWord[maxWordLength];
    size_t currentWordLen = 0;
    
    char *pos = truePositives;
    for (signed char searchingTruePositives = 1; searchingTruePositives > -1; searchingTruePositives--) {
        while (*pos) {
            if (inWord) {
                if (isalpha(*pos)) {
                    currentWord[currentWordLen++] = tolower(*pos);
                }
                else {  //we're at the end of a word!
                    currentWord[currentWordLen] = 0;
                    unsigned char foundWord = 0;
                    //check if this word is in the list
                    for (int i = 0; i < wordCount; i++) {
                        if (!strcmp(words[i].text, currentWord)) {
                            //this word is in the list.
                            //increment it's count
                            if (searchingTruePositives) {
                                words[i].trueOccurences++;
                            }
                            else {
                                words[i].falseOccurences++;
                            }
                            foundWord = 1;
                        }
                    }
                    if (!foundWord) {
                        words = realloc(words, ++wordCount * sizeof(Word));
                        words[wordCount-1].text = malloc(currentWordLen + 1);
                        strcpy(words[wordCount-1].text, currentWord);
                        words[wordCount-1].trueOccurences = searchingTruePositives;
                        words[wordCount-1].falseOccurences = !searchingTruePositives;
                    }
                    currentWordLen = 0;
                    inWord = 0;
                }
            }
            else {
                switch (*pos) {
                    case '<':   //skip HTML tags
                        if (strstr(pos, "<pre><code>") == pos) {
                            pos = strstr(pos, "</code></pre>");
                            break;  //Completely skip over code.
                        }
                        pos = strchr(pos, '>');
                        break;
                    default:
                        if (isalpha(*pos)) {
                            inWord = 1;
                            currentWordLen = 1;
                            currentWord[0] = tolower(*pos);
                        }
                        break;
                }
            }
            pos++;
        }
        pos = falsePositives;
    }
    free(truePositives);
    free(falsePositives);
    
    puts("Filter additions:");
    puts("            Word    TP rate      TPs     FPs\n");
    for (unsigned i = 0; i < wordCount; i++) {
        Word word = words[i];
        unsigned trueOccurences = word.trueOccurences;
        unsigned falseOccurences = word.falseOccurences;
        float totalOccurences = trueOccurences + falseOccurences;
        float trueRate = trueOccurences / totalOccurences;
        const char *text = word.text;
        if (totalOccurences > 5 && trueRate > 0.7) {
            unsigned char matchesExistingFilter = 0;
            for (int i = 0; i < bot->filterCount; i++) {
                if (strstr(text, bot->filters[i]->filter)) {
                    matchesExistingFilter = 1;
                }
            }
            if (!matchesExistingFilter && strlen(text) > 2) {
                Filter *newFilter = createFilter(text, text, 0, trueOccurences, falseOccurences);
                bot->filters = realloc(bot->filters, ++bot->filterCount * sizeof(Filter*));
                bot->filters[bot->filterCount - 1] = newFilter;
                printf("%16s\t%4f\t%4d\t%4d\n", text, trueRate, trueOccurences, falseOccurences);
            }
        }
    }
}

static Report **parseReports(ChatBot *bot, cJSON *json) {
    Report **reports = malloc(REPORT_MEMORY * sizeof(reports));
    bot->reportsUntilAnalysis = REPORT_MEMORY;
    if (json == NULL) {
        loadNullReports(reports);
        return reports;
    }
    
    cJSON *array = cJSON_GetObjectItem(json, "latestReports");
    
    if (cJSON_GetArraySize(array) != REPORT_MEMORY) {
        fputs("Report file doesn't have enough reports!  Ignoring report file.\n", stderr);
        loadNullReports(reports);
        cJSON_Delete(json);
        return reports;
    }
    
    bot->reportsUntilAnalysis = cJSON_GetObjectItem(json, "reportsUntilAnalysis")->valueint;
    
    for (int i = 0; i < REPORT_MEMORY; i++) {
        cJSON *data = cJSON_GetArrayItem(array, i);
        if (data->type == cJSON_NULL) {
            reports[i] = NULL;
            continue;
        }
        
        unsigned long messageID = cJSON_GetObjectItem(data, "messageID")->valueint;
        unsigned long postID = cJSON_GetObjectItem(data, "postID")->valueint;
        unsigned long userID = cJSON_GetObjectItem(data, "userID")->valueint;
        unsigned char isAnswer = cJSON_GetObjectItem(data, "isAnswer")->type == cJSON_True;
        int confirmation = cJSON_GetObjectItem(data, "confirmation")->valueint;
        const char *title = cJSON_GetObjectItem(data, "title")->valuestring;
        const char *body = cJSON_GetObjectItem(data, "body")->valuestring;
        
        Report *report = malloc(sizeof(Report));
        
        report->messageID = messageID;
        report->post = createPost(title, body, postID, isAnswer, userID);
        report->confirmation = confirmation;;
        
        reports[i] = report;
    }
    
    cJSON_Delete(json);
    return reports;
}

ChatBot *createChatBot(ChatRoom *room, Command **commands, cJSON *latestReports, Filter **filters) {
    ChatBot *c = malloc(sizeof(ChatBot));
    c->room = room;
    c->commands = commands;
    c->runningCommands = NULL;
    c->apiFilter = NULL;
    c->runningCommandCount = 0;
    c->stopAction = ACTION_NONE;
    pthread_mutex_init(&c->runningCommandsLock, NULL);
    pthread_mutex_init(&c->detectorLock, NULL);
    
    c->filters = NULL;
    c->filterCount = 0;
    
    c->reportsWaiting = -1;
    
    while (*(filters++)) {
        c->filters = realloc(c->filters, ++c->filterCount * sizeof(Filter*));
        c->filters[c->filterCount-1] = *(filters - 1);
    }
    
    Report **reports = parseReports(c, latestReports);
    for (int i = 0; i < REPORT_MEMORY; i++) {
        c->latestReports[i] = reports[i];
    }
    free(reports);
    
    return c;
}

void runCommand(ChatBot *bot, ChatMessage *message, char *command) {
    //Get the space-separated components of this command.
    char **components = NULL;
    size_t componentCount = 0;
    
    char *component;
    while ((component = strsep(&command, " "))) {
        //add command to components
        components = realloc(components, (++componentCount) * sizeof(char*));
        components[componentCount-1] = malloc(strlen(component) + 1);
        strcpy(components[componentCount-1], component);
    };
    pthread_mutex_lock(&bot->runningCommandsLock);
    RunningCommand *c = launchCommand(message, (int)componentCount, components, bot->commands, bot);
    bot->runningCommands = realloc(bot->runningCommands, ++bot->runningCommandCount * sizeof(RunningCommand *));
    bot->runningCommands[bot->runningCommandCount-1] = c;
    pthread_mutex_unlock(&bot->runningCommandsLock);
}

void prepareCommand(ChatBot *bot, ChatMessage *message, char *messageText) {
    char *command = strchr(messageText, ' ');
    if (command) {
        while (*(++command) == ' ');
        if (*command && bot->stopAction == ACTION_NONE) {
            runCommand(bot, message, command);
            return;
        }
    }
}

Report *reportWithMessage(ChatBot *bot, unsigned long messageID) {
    for (int i = 0; i < REPORT_MEMORY; i++) {
        if (bot->latestReports[i]) {
            if (messageID == bot->latestReports[i]->messageID) {
                return bot->latestReports[i];
            }
        }
    }
    return NULL;
}

void processMessage(ChatBot *bot, ChatMessage *message) {
    char *messageText = malloc(strlen(message->content) + 1);
    strcpy(messageText, message->content);
    if ((strstr(messageText, "@Fire") == messageText) || (strstr(messageText, "@FireAlarm") == messageText)) {
        //messageText starts with "@Bot"
        prepareCommand(bot, message, messageText);
        
    }
    else if (bot->reportsWaiting != -1 && strstr(messageText, REPORT_HEADER)) {
        bot->latestReports[bot->reportsWaiting--]->messageID = message->id;
        deleteChatMessage(message);
    }
    else if (message->replyID && reportWithMessage(bot, message->replyID)) {
        prepareCommand(bot, message, messageText);
    }
    else {
        deleteChatMessage(message);
    }
    free(messageText);
}

Post *getPostByID(ChatBot *bot, unsigned long postID) {
    pthread_mutex_lock(&bot->room->clientLock);
    CURL *curl = bot->room->client->curl;
    
    checkCURL(curl_easy_setopt(curl, CURLOPT_HTTPGET, 1));
    OutBuffer buffer;
    buffer.data = NULL;
    checkCURL(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer));
    
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
    
    if (bot->apiFilter == NULL) {
        checkCURL(curl_easy_setopt(curl, CURLOPT_URL,
                                   "api.stackexchange.com/2.2/filters/create"
                                   "?include=post.title;post.body;question.tags;user.user_id;&unsafe=false&key="API_KEY
                                   ));
        checkCURL(curl_easy_perform(curl));
        
        cJSON *json = cJSON_Parse(buffer.data);
        free(buffer.data);
        buffer.data = NULL;
        
        cJSON *items = cJSON_GetObjectItem(json, "items");
        char *filter = cJSON_GetObjectItem(cJSON_GetArrayItem(items, 0), "filter")->valuestring;
        bot->apiFilter = malloc(strlen(filter) + 1);
        strcpy(bot->apiFilter, filter);
        cJSON_Delete(json);
    }
    
    
    
    unsigned max = 256;
    char request[max];
    snprintf(request, max,
             "https://api.stackexchange.com/posts/%lu?site=stackoverflow&filter=%s&key="API_KEY,
             postID, bot->apiFilter
             );
    curl_easy_setopt(curl, CURLOPT_URL, request);
    
    
    
    checkCURL(curl_easy_perform(curl));
    
    checkCURL(curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""));
    
    
    pthread_mutex_unlock(&bot->room->clientLock);
    
    cJSON *json = cJSON_Parse(buffer.data);
    free(buffer.data);
    
    if (cJSON_GetObjectItem(json, "error_id")) {
        cJSON_Delete(json);
        puts("Error fetching post!");
        return NULL;
    }
    
    cJSON *postJSON = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "items"), 0);
    if (postJSON == NULL) {
        cJSON_Delete(json);
        return NULL;
    }
    
    //puts(cJSON_Print(postJSON));
    
    
    char *title = cJSON_GetObjectItem(postJSON, "title")->valuestring;
    char *body = cJSON_GetObjectItem(postJSON, "body")->valuestring;
    char *type = cJSON_GetObjectItem(postJSON, "post_type")->valuestring;
    unsigned userID = cJSON_GetObjectItem(cJSON_GetObjectItem(postJSON, "owner"), "user_id")->valueint;
    
    Post *p = createPost(title, body, postID, strcmp(type, "answer") == 0, userID);
    
    cJSON_Delete(json);
    return p;
}

void checkPost(ChatBot *bot, Post *post) {
    unsigned likelihood = 0;
    char *messageBuf = malloc(sizeof(char));
    *messageBuf = 0;
    for (int i = 0; i < bot->filterCount; i++) {
        unsigned start, end;
        if (postMatchesFilter(post, bot->filters[i], &start, &end)) {
            
            const char *desc = bot->filters[i]->desc;
            messageBuf = realloc(messageBuf, strlen(messageBuf) + strlen(desc) + 16);
            
            snprintf(messageBuf + strlen(messageBuf), strlen(desc) + 16,
                     "%s%s", (likelihood ? ", " : ""), desc);
            //If this not the first match, start it with a comma and space.
            
            const float truePositives = bot->filters[i]->truePositives;
            likelihood += (truePositives / (truePositives + bot->filters[i]->falsePositives)) * 1000;
        }
    }
    if (likelihood > THRESHOLD) {
        const size_t maxMessage = strlen(messageBuf) + 256;
        char *message = malloc(maxMessage);
        snprintf(message, maxMessage,
                 REPORT_HEADER " (%s): [%s](http://stackoverflow.com/%s/%lu) (likelihood %d)",
                 messageBuf, post->title, post->isAnswer ? "a" : "q", post->postID, likelihood);
        
        postMessage(bot->room, message);
        
        if (bot->latestReports[REPORT_MEMORY-1]) {
            free(bot->latestReports[REPORT_MEMORY-1]->post);
            free(bot->latestReports[REPORT_MEMORY-1]);
        }
        int i = REPORT_MEMORY;
        while(--i) {
            bot->latestReports[i] = bot->latestReports[i-1];
        }
        Report *report = malloc(sizeof(Report));
        report->post = post;
        report->confirmation = -1;
        bot->latestReports[0] = report;
        bot->reportsWaiting++;
        bot->reportsUntilAnalysis--;
        if (bot->reportsUntilAnalysis == 0) {
            bot->reportsUntilAnalysis = REPORT_MEMORY;
            analyzeReports(bot);
        }
        free(message);
    }
    else {
        deletePost(post);
    }
    
    free(messageBuf);
}

void confirmPost(ChatBot *bot, Post *post, unsigned char confirmed) {
    for (int i = 0; i < bot->filterCount; i++) {
        Filter *filter = bot->filters[i];
        if (postMatchesFilter(post, filter, NULL, NULL)) {
            if (confirmed) {
                filter->truePositives++;
            }
            else {
                filter->falsePositives++;
            }
            printf("Increased %s positive count of %s.\n", confirmed ? "true" : "false", filter->desc);
        }
    }
}

StopAction runChatBot(ChatBot *c) {
    ChatMessage **messages = processChatRoomEvents(c->room);
    ChatMessage *message;
    for (int i = 0; (message = messages[i]); i++) {
        processMessage(c, message);
    }
    free(messages);
    
    //clean up old commands
    for (int i = 0; i < c->runningCommandCount; i++) {
        if (c->runningCommands[i]->finished) {
            //delete the command
            c->runningCommandCount--;
            int j = i;
            for (deleteRunningCommand(c->runningCommands[j]); j < c->runningCommandCount; j++) {
                c->runningCommands[i] = c->runningCommands[i+1];
            }
            c->runningCommands = realloc(c->runningCommands, c->runningCommandCount * sizeof(RunningCommand*));
        }
    }
    if (c->stopAction != ACTION_NONE) {
        if (c->room->pendingMessageLinkedList == NULL && (c->runningCommandCount == 0)) {
            return c->stopAction;
        }
    }
    
    return ACTION_NONE;
}
