#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <unistd.h>

struct msgbuf {
    long m_type;
    struct message_ {
        int pid;
    } message;
};

struct schedMsg {
    long m_type;
    int message;
};


int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <MQ1_id> <MQ2_id> <k>\n", argv[0]);
        exit(1);
    }
    int MQ1_id = atoi(argv[1]);
    int MQ2_id = atoi(argv[2]);
    int k = atoi(argv[3]);

    struct msgbuf msg;
    struct schedMsg schMsg;

    int cnt = 0;
    while (cnt < k) {

        // Select first process from ready queue (FCFS)
        msgrcv(MQ1_id, &msg, sizeof(msg.message), 1, 0);

        // Signal the process to start
        kill(msg.message.pid, SIGUSR1);

        // Get the response from MMU
        msgrcv(MQ2_id, &schMsg, sizeof(schMsg.message), 1, 0);

        if (schMsg.message == 1) {
            // Type I message: PAGE FAULT HANDLED
            // Enqueue the current process back into the ready queue
            msg.m_type = 1;
            msgsnd(MQ1_id, &msg, sizeof(msg.message), 0);
        } else {
            // Type II message: PROCESS TERMINATED
            // Process completed or encountered invalid page; schedule next
            ++cnt;
        }
    }
    // Signal the Master process that all processes have finished
    kill(getppid(), SIGUSR1);

    pause();
    return 0;
}
