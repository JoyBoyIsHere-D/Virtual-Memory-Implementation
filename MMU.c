#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>

int timestamp = 0; // Global timestamp(count)

typedef struct pageTable_ {
    int valid;
    int frame_no;
} pageTable;

typedef struct pageMap_ {
    int pid;
    int num_page;
} pageMap;

struct pageFrame {
    long m_type;
    struct message {
        int pid;
        int data;
    } message;
};

struct schedMsg {
    long m_type;
    int message;
};

void PageFaultHandler(int p_i, int m, int f, int page_num, pageTable *page_table, int *free_frame_list, int pageTimeStamp[][m]) {

    // Check if free frame is available in main memory
    for (int i = 0; i < f; i++) {
        if (free_frame_list[i] == 1) {
            page_table[p_i * m + page_num].frame_no = i;
            page_table[p_i * m + page_num].valid = 1;
            free_frame_list[i] = 0;
            pageTimeStamp[p_i][page_num] = timestamp; // Record initial allocation timestamp
            return;
        }
    }

    // Perform local page replacement using LRU (Least Recently Used)
    int diff;
    int lruPage = -1;
    int maxDiff = -1;
    for (int j = 0; j < m; j++) {
        if (page_table[p_i * m + j].valid == 1) {
            diff = timestamp - pageTimeStamp[p_i][j];

            if (diff > maxDiff) {
                maxDiff = diff;
                lruPage = j;
            }
        }
    }

    // If a victim page is found, replace it
    if (maxDiff != -1 && lruPage != -1) {
        // Evict LRU victim page
        page_table[p_i * m + page_num].frame_no = page_table[p_i * m + lruPage].frame_no;
        page_table[p_i * m + lruPage].valid = 0;
        page_table[p_i * m + lruPage].frame_no = -1;
        pageTimeStamp[p_i][lruPage] = -1;

        // Bring in new page
        page_table[p_i * m + page_num].valid = 1;
        pageTimeStamp[p_i][page_num] = timestamp;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 9) {
        fprintf(stderr, "Usage: %s <MQ2_id> <MQ3_id> <SM1_id> <SM2_id> <SM3_id> <k> <m> <f>\n", argv[0]);
        exit(1);
    }

    int MQ2_id = atoi(argv[1]);
    int MQ3_id = atoi(argv[2]);
    int SM1_id = atoi(argv[3]);
    int SM2_id = atoi(argv[4]);
    int SM3_id = atoi(argv[5]);
    int k = atoi(argv[6]);
    int m = atoi(argv[7]);
    int f = atoi(argv[8]);

    // Open output file (truncate if already present)
    int fd = open("result.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("Failed to open result.txt");
        exit(1);
    }

    // Attach shared memory segments
    pageTable *page_table = (pageTable *)shmat(SM1_id, NULL, 0);
    int *free_frame_list = (int *)shmat(SM2_id, NULL, 0);
    pageMap *page_map = (pageMap *)shmat(SM3_id, NULL, 0);

    // Initialise pageTimeStamp to store timestamp of each page allotment/access
    int pageTimeStamp[k][m];
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < m; j++) {
            if (page_table[i * m + j].valid == 1) {
                pageTimeStamp[i][j] = timestamp;
            } else {
                pageTimeStamp[i][j] = -1;
            }
        }
    }

    // Page faults and Invalid page reference counters
    int page_faults[k], invalid_page_ref[k];
    for (int i = 0; i < k; i++) {
        page_faults[i] = 0;
        invalid_page_ref[i] = 0;
    }

    struct schedMsg schMsg;
    struct pageFrame pgf_msg;

    int cnt = 0;

    while (cnt < k) {

        msgrcv(MQ3_id, &pgf_msg, sizeof(pgf_msg.message), 1, 0);

        int page_num = pgf_msg.message.data;
        int p_pid = pgf_msg.message.pid;

        // Find the index of the requesting process
        int p_i = -1;
        for (int i = 0; i < k; i++) {
            if (page_map[i].pid == p_pid) {
                p_i = i;
                break;
            }
        }

        if (p_i == -1) {
            fprintf(stderr, "MMU Warning: Received message from unrecognized PID %d\n", p_pid);
            continue;
        }

        int process_num = p_i + 1;

        // Print the global ordering
        char str[100];
        memset(str, '\0', 100);
        printf("Global ordering - (%d,%d,%d)\n", timestamp, process_num, page_num);
        sprintf(str, "Global ordering - (%d,%d,%d)\n", timestamp, process_num, page_num);
        write(fd, str, strlen(str));

        // Case 1: Process has completed its reference string (page_num == -9)
        if (page_num == -9) {
            // Free all frames allocated to this process
            for (int j = 0; j < m; j++) {
                if (page_table[p_i * m + j].valid == 1) {
                    free_frame_list[page_table[p_i * m + j].frame_no] = 1;
                    page_table[p_i * m + j].valid = 0;
                    page_table[p_i * m + j].frame_no = -1;
                }
            }

            // Send Type II message (TERMINATED) to scheduler
            schMsg.m_type = 1;
            schMsg.message = 2;
            msgsnd(MQ2_id, &schMsg, sizeof(schMsg.message), 0);
            cnt++;
        }
        // Case 2: Illegal page number requested (segmentation fault)
        else if (page_map[p_i].num_page <= page_num || page_num < 0) {
            // Print the Invalid page reference
            printf("Invalid page reference - (%d,%d)\n", process_num, page_num);
            memset(str, '\0', 100);
            sprintf(str, "Invalid page reference - (%d,%d)\n", process_num, page_num);
            write(fd, str, strlen(str));

            // Notify process of termination due to illegal address (-2)
            pgf_msg.m_type = p_pid;
            pgf_msg.message.data = -2;
            msgsnd(MQ3_id, &pgf_msg, sizeof(pgf_msg.message), 0);
            invalid_page_ref[p_i]++;

            // Free all frames allocated to this process
            for (int j = 0; j < m; j++) {
                if (page_table[p_i * m + j].valid == 1) {
                    free_frame_list[page_table[p_i * m + j].frame_no] = 1;
                    page_table[p_i * m + j].valid = 0;
                    page_table[p_i * m + j].frame_no = -1;
                }
            }

            // Send Type II message (TERMINATED) to scheduler
            schMsg.m_type = 1;
            schMsg.message = 2;
            msgsnd(MQ2_id, &schMsg, sizeof(schMsg.message), 0);
            cnt++;
        }
        // Case 3: Legal page number requested
        else {
            if (page_table[p_i * m + page_num].valid == 1) {
                // Page Hit: Page is present in memory
                pageTimeStamp[p_i][page_num] = timestamp;

                // Send frame number back to process
                pgf_msg.m_type = p_pid;
                pgf_msg.message.data = page_table[p_i * m + page_num].frame_no;
                msgsnd(MQ3_id, &pgf_msg, sizeof(pgf_msg.message), 0);
            } else {
                // Page Fault: Page not in memory
                printf("Page fault sequence - (%d,%d)\n", process_num, page_num);
                memset(str, '\0', 100);
                sprintf(str, "Page fault sequence - (%d,%d)\n", process_num, page_num);
                write(fd, str, strlen(str));

                // Handle page fault (allocate free frame or LRU replacement)
                PageFaultHandler(p_i, m, f, page_num, page_table, free_frame_list, pageTimeStamp);
                page_faults[p_i]++;

                // Send -1 to process indicating page fault occurred
                pgf_msg.m_type = p_pid;
                pgf_msg.message.data = -1;
                msgsnd(MQ3_id, &pgf_msg, sizeof(pgf_msg.message), 0);

                // Send Type I message (PAGE FAULT HANDLED) to scheduler
                schMsg.m_type = 1;
                schMsg.message = 1;
                msgsnd(MQ2_id, &schMsg, sizeof(schMsg.message), 0);
            }
        }

        timestamp++;
    }

    // Print summary of page faults and invalid references
    for (int i = 0; i < k; i++) {
        printf("Process %d: No. of Page faults = %d, No. of Invalid page references = %d\n", i + 1, page_faults[i], invalid_page_ref[i]);
        char str[500];
        memset(str, '\0', 500);
        sprintf(str, "Process %d: No. of Page faults = %d, No. of Invalid page references = %d\n", i + 1, page_faults[i], invalid_page_ref[i]);
        write(fd, str, strlen(str));
    }

    close(fd);
    shmdt(page_table);
    shmdt(free_frame_list);
    shmdt(page_map);

    pause();
    return 0;
}
