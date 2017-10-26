// scheduler.h - the muwerk simple scheduler

#pragma once

#include "../ustd/platform.h"
#include "../ustd/array.h"
#include "../ustd/queue.h"

#include <stdio.h>

#if defined(__ESP__) || defined(__UNIXOID__)
#include <functional>
#endif

namespace ustd {

enum T_PRIO {
    PRIO_SYSTEMCRITICAL = 0,
    PRIO_TIMECRITICAL = 1,
    PRIO_HIGH = 2,
    PRIO_NORMAL = 3,
    PRIO_LOW = 4,
    PRIO_LOWEST = 5
};

enum T_MSGTYPE {
    MSG_NONE = 0,
    MSG_DIRECT = 1,
    MSG_SUBSCRIBE = 2,
    MSG_UNSUBSCRIBE = 3,
    MSG_PUBLISH = 4,
    MSG_PUBLISHRAW = 5
};

typedef void (*T_TASK)();

typedef struct {
    char *topic;  // topic string
    char *msg;
} T_MSG;

#if defined(__ESP__) || defined(__UNIXOID__)
typedef std::function<void(String topic, String msg)> T_SUBS;
#else
typedef void (*T_SUBS)(String topic, String msg);
#endif

typedef struct {
    int subscriptionHandle;
    char *topic;
    T_SUBS subs;
} T_SUBSCRIPTION;

typedef struct {
    T_TASK task;
    T_PRIO prio;
    unsigned long minMicros;
    unsigned long lastCall;
    unsigned long lateTime;
} T_TASKENTRY;

class Scheduler {
  private:
    ustd::array<T_TASKENTRY> taskList;
    ustd::array<T_SUBSCRIPTION> subscriptionList;
    unsigned int subscriptionHandle;
    ustd::queue<T_MSG> msgqueue;

  public:
    Scheduler(int nTaskListSize = 2, int queueSize = 2,
              int nSubscriptionListSize = 2)
        : taskList(nTaskListSize), msgqueue(queueSize),
          subscriptionList(nSubscriptionListSize) {
        subscriptionHandle = 0;
#ifdef __ESP__
        ESP.wdtDisable();
        ESP.wdtEnable(WDTO_8S);
#endif
    }

#ifndef __ATTINY__
    virtual ~Scheduler() {
        int l = msgqueue.length();
        for (int i = 0; i < l; i++) {
            msgqueue.pop();
        }
    }
#endif
    bool mqttmatch(const String pubstr, const String substr) {
        // compares publish and subscribe topics.
        // subscriptions can contain the MQTT wildcards '#' and '+'.
        if (pubstr == substr)
            return true;

        // Attiny compile: core needs to be extended, add c_str():
        // In WString.h add:  const char *c_str() const { return buffer; }
        const char *pub = (const char *)pubstr.c_str();
        const char *sub = (const char *)substr.c_str();

        int lp = strlen(pub);
        int ls = strlen(sub);

        bool wPos = true;  // sub wildcard is legal now
        int ps = 0;
        for (int pp = 0; pp < lp; pp++) {
            // if ( pp >= ls || ps > ls ) {
            //    DBG( "Pub more spec than sub: " + String( pp ) + "," +
            //    String( ps ) ); return false; // Pub is more specific than
            //    sub
            // }
            if (pub[pp] == '+' || pub[pp] == '#') {
                return false;  // Illegal wildcards in pub
            }
            if (wPos) {
                wPos = false;
                if (sub[ps] == '#') {
                    if (ps == ls - 1) {
                        return true;
                    } else {
                        return false;  // In sub, # must not be followed by
                                       // anything else
                    }
                }
                if (sub[ps] == '+') {
                    while (pp < lp && pub[pp] != '/')
                        ++pp;
                    ++ps;
                    if (pp == lp) {
                        if (ps == ls) {
                            return true;
                        } else if (!strcmp(&sub[ps], "/#")) {
                            return true;
                        }
                    }
                }
            } else {
                if (sub[ps] == '+' || sub[ps] == '#') {
                    return false;  // Illegal wildcard-position
                }
            }
            if (pub[pp] != sub[ps] && strcmp(&sub[ps], "/#")) {
                return false;
            }
            if (pub[pp] == '/')
                wPos = true;
            if (pp == lp - 1) {
                if (ps == ls - 1) {
                    return true;
                }
                if (!strcmp(&sub[ps + 1], "/#") || !strcmp(&sub[ps + 1], "#") ||
                    !strcmp(&sub[ps + 1], "+")) {
                    return true;
                }
                return false;
            }
            ++ps;
        }
        if (ps == ls) {
            return true;
        } else {
            return false;
        }
    }

    bool publish(String topic, String msg) {
        T_MSG *pMsg = (T_MSG *)malloc(sizeof(T_MSG));
        memset(pMsg, 0, sizeof(T_MSG));
        pMsg->msg = (char *)malloc(msg.length() + 1);
        pMsg->topic = (char *)malloc(topic.length() + 1);
        strcpy(pMsg->topic, topic.c_str());
        strcpy(pMsg->msg, msg.c_str());
        return msgqueue.push(pMsg);
    }

    int subscribe(String topic, T_SUBS subs) {
        T_SUBSCRIPTION sub;
        memset(&sub, 0, sizeof(sub));
        sub.topic = (char *)malloc(topic.length() + 1);
        strcpy(sub.topic, topic.c_str());
        sub.subs = subs;
        ++subscriptionHandle;
        sub.subscriptionHandle = subscriptionHandle;
        if (subscriptionList.add(sub) == -1)
            return -1;
        else
            return subscriptionHandle;
    }

    // give a c++11 lambda as callback for incoming mqttmessages:
    // std::function<void(char *, unsigned char *, unsigned int)> f =
    //    [=](char *t, unsigned char *m, unsigned int l) {
    //        this->onMqttReceive(t, m, l);
    //    };
    // mqttClient.setCallback(f);

    bool unsubscribe(int subscriptionHandle) {
        for (unsigned int i = 0; i < subscriptionList.length(); i++) {
            if (subscriptionList[i].subscriptionHandle == subscriptionHandle) {
                free(subscriptionList[i].topic);
                subscriptionList.erase(i);
                return true;
            }
        }
        return false;
    }

    void checkMsgQueue() {
        T_MSG *pMsg;
        while ((pMsg = msgqueue.pop()) != nullptr) {
            for (unsigned int i = 0; i < subscriptionList.length(); i++) {
                if (mqttmatch(pMsg->topic, subscriptionList[i].topic)) {
                    subscriptionList[i].subs(pMsg->topic, pMsg->msg);
                }
            }
            free(pMsg->topic);
            free(pMsg->msg);
            free(pMsg);
        }
    }

    void runTask(T_TASKENTRY *pTaskEnt) {
        unsigned long ticker = micros();
        unsigned long tDelta = timeDiff(pTaskEnt->lastCall, ticker);
        if (tDelta >= pTaskEnt->minMicros) {
            pTaskEnt->task();
            pTaskEnt->lastCall = micros();
            pTaskEnt->lateTime += tDelta - pTaskEnt->minMicros;
        }
    }

    void loop() {
        checkMsgQueue();
        for (unsigned int i = 0; i < taskList.length(); i++) {
            checkMsgQueue();
            runTask(&taskList[i]);
#ifdef __ESP__
            yield();
#endif
        }
#ifdef __ESP__
        ESP.wdtFeed();
#endif
    }

    bool add(T_TASK task, unsigned long minMicroSecs = 100000L,
             T_PRIO prio = PRIO_NORMAL) {

        T_TASKENTRY taskEnt;
        memset(&taskEnt, 0, sizeof(taskEnt));
        taskEnt.task = task;
        taskEnt.minMicros = minMicroSecs;
        taskEnt.prio = prio;

        if (taskList.add(taskEnt) >= 0)
            return true;
        else
            return false;
    }

    bool remove(T_TASK task) {
        for (unsigned int i = 0; i < taskList.length(); i++) {
            if (taskList[i].task == task) {
                taskList.erase(i);
                return true;
            }
        }
        return false;
    }
};
}  // namespace ustd