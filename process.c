#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <unistd.h>

static volatile sig_atomic_t signal_msg = 0;

void signalHandler(int sig) {
    if (sig == SIGUSR1) {
        signal_msg = 1; // Signal received from scheduler
    }
}

static void waitForSchedule(void) {
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);
    while (signal_msg == 0) {
        sigsuspend(&oldmask);
    }
    signal_msg = 0;
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

struct msgbuf {
    long m_type;
    struct message_ {
        int pid;
    } message;
};

struct pageFrame {
    long m_type;
    struct message {
        int pid;
        int data;
    } message;
};

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <MQ1_id> <MQ3_id> <reference_string>\n", argv[0]);
        exit(1);
    }

    int MQ1_id = atoi(argv[1]);
    int MQ3_id = atoi(argv[2]);

    char *ref_str = argv[3];
    int process_id = getpid();

    struct msgbuf msg;
    msg.m_type = 1;
    msg.message.pid = process_id;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigaction(SIGUSR1, &sa, NULL);

    // Block SIGUSR1 before enqueuing to ready queue to avoid missing immediate signals
    sigset_t block_mask, orig_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block_mask, &orig_mask);

    // Send the process id to the scheduler (ready queue)
    msgsnd(MQ1_id, &msg, sizeof(msg.message), 0);

    // Wait until scheduler selects this process for the first time
    while (signal_msg == 0) {
        sigsuspend(&orig_mask);
    }
    signal_msg = 0;
    sigprocmask(SIG_UNBLOCK, &block_mask, NULL);

    struct pageFrame pgf_msg;

    char *token = strtok(ref_str, ",");
    while (token != NULL) {
        int page_no = atoi(token);

        // Send the page request to MMU
        pgf_msg.m_type = 1;
        pgf_msg.message.pid = process_id;
        pgf_msg.message.data = page_no;
        msgsnd(MQ3_id, &pgf_msg, sizeof(pgf_msg.message), 0);

        // Wait for the response from MMU
        msgrcv(MQ3_id, &pgf_msg, sizeof(pgf_msg.message), process_id, 0);

        if (pgf_msg.message.data >= 0) {
            // MMU sent a valid frame number (Page Hit or successfully serviced)
            // Parse next page number in the reference string
            token = strtok(NULL, ",");
        } else if (pgf_msg.message.data == -1) {
            // MMU sent a page fault (-1)
            // Save current element (do not advance token) and wait until rescheduled
            waitForSchedule();
            // Next iteration will retry the same page number, which is now loaded into memory
        } else if (pgf_msg.message.data == -2) {
            // MMU sent illegal page access signal (-2)
            exit(0);
        } else {
            perror("Invalid data received from MMU");
            exit(-1);
        }
    }

    // Send the completion marker (-9) to MMU
    pgf_msg.m_type = 1;
    pgf_msg.message.pid = process_id;
    pgf_msg.message.data = -9;
    msgsnd(MQ3_id, &pgf_msg, sizeof(pgf_msg.message), 0);

    return 0;
}
