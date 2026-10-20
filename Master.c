#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const int STR_SIZE = 5000;
const float prob = 0.05; // Probability of illegal page number
static volatile sig_atomic_t waitSched = 1; // Wait for scheduler to finish

typedef struct pageTable_ {
    int valid;
    int frame_no;
} pageTable;

typedef struct pageMap_ {
    int pid;
    int num_page;
} pageMap;

// Global IPC and child identifiers for cleanup on exit or SIGINT
static int g_SM1_id = -1, g_SM2_id = -1, g_SM3_id = -1;
static int g_MQ1_id = -1, g_MQ2_id = -1, g_MQ3_id = -1;
static pid_t g_sched_pid = -1, g_mmu_pid = -1;
static pageTable *g_page_table = NULL;
static int *g_free_frame_list = NULL;
static pageMap *g_page_map = NULL;

void signalHandler(int sig) {
    //SIGUSR1: The "It's your turn to work!" chime.
    if (sig == SIGUSR1) {
        waitSched = 0;
    }
}

void cleanup_ipc(void) {
    //"I am done reading and writing to this memory for now. Disconnect my pointer."
    if (g_page_map && g_page_map != (void *)-1) shmdt(g_page_map);
    if (g_page_table && g_page_table != (void *)-1) shmdt(g_page_table);
    if (g_free_frame_list && g_free_frame_list != (void *)-1) shmdt(g_free_frame_list);

    //"We are completely finished with this project. Destroy this shared memory block and free the RAM."
    if (g_SM1_id != -1) shmctl(g_SM1_id, IPC_RMID, NULL);
    if (g_SM2_id != -1) shmctl(g_SM2_id, IPC_RMID, NULL);
    if (g_SM3_id != -1) shmctl(g_SM3_id, IPC_RMID, NULL);

    //"We are finished running the project. Delete this mailbox and throw away any leftover letters."
    if (g_MQ1_id != -1) msgctl(g_MQ1_id, IPC_RMID, NULL);
    if (g_MQ2_id != -1) msgctl(g_MQ2_id, IPC_RMID, NULL);
    if (g_MQ3_id != -1) msgctl(g_MQ3_id, IPC_RMID, NULL);
}

void sigint_handler(int sig) {
    // SIGINT: The "Emergency! Shut everything down!" chime (like pressing Ctrl + C).
    (void)sig;
    //Kill does nothing but send a signal to the process with the given PID.
    // If the process is already terminated, it will have no effect.
    if (g_sched_pid > 0) kill(g_sched_pid, SIGINT);
    if (g_mmu_pid > 0) kill(g_mmu_pid, SIGINT);
    cleanup_ipc();
    exit(1);
}

int main() {
    int k, m, f;
    printf("Enter the total number of processes: ");
    if (scanf("%d", &k) != 1 || k <= 0) {
        fprintf(stderr, "Invalid number of processes.\n");
        return 1;
    }
    printf("Enter the Virtual address space - Maximum number of pages required per process: ");
    if (scanf("%d", &m) != 1 || m <= 0) {
        fprintf(stderr, "Invalid virtual address space size.\n");
        return 1;
    }
    printf("Enter the physical address space - Total number of frames: ");
    if (scanf("%d", &f) != 1 || f <= 0) {
        fprintf(stderr, "Invalid physical address space size.\n");
        return 1;
    }

    // Set signal handlers for scheduler notification and graceful termination
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    // "When SIGUSR1 rings, don't shut me down! Instead,
    // run my custom function named signalHandler()."
    sigaction(SIGUSR1, &sa, NULL);

    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa_int, NULL);

    // Initialise the page table for k processes
    key_t SM1_key = ftok("/tmp", 100); // Generate a unique key for shared memory segment 1
    int SM1_id = shmget(SM1_key, k * m * sizeof(pageTable), IPC_CREAT | 0666); 
    // Create shared memory segment for page table
    // Basically reserve memory for k processes, each with m pages, and each page having a pageTable structure
    if (SM1_id < 0) {
        perror("shmget SM1 failed");
        cleanup_ipc();
        exit(1);
    }
    g_SM1_id = SM1_id;

    // Initialise the free frame list
    key_t SM2_key = ftok("/tmp", 101);
    int SM2_id = shmget(SM2_key, f * sizeof(int), IPC_CREAT | 0666);
    if (SM2_id < 0) {
        perror("shmget SM2 failed");
        cleanup_ipc();
        exit(1);
    }
    g_SM2_id = SM2_id;

    // Initialise the page map
    key_t SM3_key = ftok("/tmp", 102);
    int SM3_id = shmget(SM3_key, k * sizeof(pageMap), IPC_CREAT | 0666);
    if (SM3_id < 0) {
        perror("shmget SM3 failed");
        cleanup_ipc();
        exit(1);
    }
    g_SM3_id = SM3_id;

    // Initialise the ready queue
    key_t MQ1_key = ftok("/tmp", 103);
    //"Please give me a mailbox associated with this key. 
    //If it doesn't exist yet (IPC_CREAT), create a brand new one!"
    int MQ1_id = msgget(MQ1_key, IPC_CREAT | 0666);
    if (MQ1_id < 0) {
        perror("msgget MQ1 failed");
        cleanup_ipc();
        exit(1);
    }
    g_MQ1_id = MQ1_id;

    // Initialise the message queue between scheduler and MMU
    key_t MQ2_key = ftok("/tmp", 104);
    int MQ2_id = msgget(MQ2_key, IPC_CREAT | 0666);
    if (MQ2_id < 0) {
        perror("msgget MQ2 failed");
        cleanup_ipc();
        exit(1);
    }
    g_MQ2_id = MQ2_id;

    // Initialise the message queue between MMU and process
    key_t MQ3_key = ftok("/tmp", 105);
    int MQ3_id = msgget(MQ3_key, IPC_CREAT | 0666);
    if (MQ3_id < 0) {
        perror("msgget MQ3 failed");
        cleanup_ipc();
        exit(1);
    }
    g_MQ3_id = MQ3_id;

    pageTable *page_table = (pageTable *)shmat(SM1_id, NULL, 0); 
    //Returns a pointer to the shared memory segment for the page table
    //Now you can use it like an array
    int *free_frame_list = (int *)shmat(SM2_id, NULL, 0);
    pageMap *page_map = (pageMap *)shmat(SM3_id, NULL, 0);

    if (page_table == (void *)-1 || free_frame_list == (void *)-1 || page_map == (void *)-1) {
        perror("shmat failed");
        cleanup_ipc();
        exit(1);
    }
    g_page_table = page_table;
    g_free_frame_list = free_frame_list;
    g_page_map = page_map;

    // Set all frames as free and invalid initially
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < m; j++) {
            page_table[i * m + j].valid = 0;
            page_table[i * m + j].frame_no = -1;
        }
    }

    for (int i = 0; i < f; i++) {
        free_frame_list[i] = 1; // 1 = Frame is free
    }

    // Set all pages as not mapped initially
    for (int i = 0; i < k; i++) {
        page_map[i].pid = -1;
        page_map[i].num_page = -1;
    }

    pid_t sched_pid, mmu_pid;

    if ((sched_pid = fork()) == 0) { // Scheduler child
        char MQ1_str[STR_SIZE], MQ2_str[STR_SIZE], num_process_str[STR_SIZE];
        sprintf(MQ1_str, "%d", MQ1_id);
        sprintf(MQ2_str, "%d", MQ2_id);
        sprintf(num_process_str, "%d", k);
        execlp("./sched", "./sched", MQ1_str, MQ2_str, num_process_str, NULL);
        perror("execlp sched failed");
        exit(1);
    } else if (sched_pid < 0) {
        perror("fork sched failed");
        cleanup_ipc();
        exit(1);
    }
    g_sched_pid = sched_pid;

    if ((mmu_pid = fork()) == 0) { // MMU child
        char SM1_str[STR_SIZE], SM2_str[STR_SIZE], SM3_str[STR_SIZE], MQ2_str[STR_SIZE], MQ3_str[STR_SIZE], k_str[STR_SIZE], m_str[STR_SIZE], f_str[STR_SIZE];
        sprintf(SM1_str, "%d", SM1_id);
        sprintf(SM2_str, "%d", SM2_id);
        sprintf(SM3_str, "%d", SM3_id);
        sprintf(MQ2_str, "%d", MQ2_id);
        sprintf(MQ3_str, "%d", MQ3_id);
        sprintf(k_str, "%d", k);
        sprintf(m_str, "%d", m);
        sprintf(f_str, "%d", f);

        // Attempt running MMU inside xterm; fallback to direct execution if xterm / DISPLAY is unavailable
        execlp("xterm", "xterm", "-hold", "-e", "./mmu", MQ2_str, MQ3_str, SM1_str, SM2_str, SM3_str, k_str, m_str, f_str, NULL);
        execlp("./mmu", "./mmu", MQ2_str, MQ3_str, SM1_str, SM2_str, SM3_str, k_str, m_str, f_str, NULL);
        perror("execlp mmu failed");
        exit(1);
    } else if (mmu_pid < 0) {
        perror("fork mmu failed");
        cleanup_ipc();
        exit(1);
    }
    g_mmu_pid = mmu_pid;

    int total_pages = 0;
    // Assigning the number of pages for each process
    srand((unsigned)time(NULL));
    for (int i = 0; i < k; i++) {
        int m_i = rand() % m + 1; // Number of pages required (1 to m)
        page_map[i].num_page = m_i;
        total_pages += m_i;
    }

    int frame_cnt = 0;
    // Allocate frames proportionally to each process based on required pages
    for (int i = 0; i < k; i++) {
        if (frame_cnt >= f) {
            break;
        }
        int num_frames = (page_map[i].num_page * f) / total_pages;

        // Cap num_frames to actual pages required by process
        if (num_frames > page_map[i].num_page) {
            num_frames = page_map[i].num_page;
        }

        // Assign frames weightage wise to each process
        for (int j = 0; j < num_frames; j++) {
            if (frame_cnt >= f) {
                break;
            }
            page_table[i * m + j].frame_no = frame_cnt;
            page_table[i * m + j].valid = 1;
            free_frame_list[frame_cnt] = 0;
            frame_cnt++;
        }

        // Give at least one frame to each process if possible
        if (num_frames == 0 && frame_cnt < f && page_map[i].num_page > 0) {
            page_table[i * m].frame_no = frame_cnt;
            page_table[i * m].valid = 1;
            free_frame_list[frame_cnt] = 0;
            frame_cnt++;
        }
    }

    // Spawn k processes at fixed intervals of 250ms
    for (int i = 0; i < k; i++) {
        int m_i = page_map[i].num_page;
        int x = 2 * m_i + rand() % (8 * m_i + 1); // Length of reference string: [2*m_i, 10*m_i]
        
        // Generate the reference string
        char ref_str[STR_SIZE];
        memset(ref_str, '\0', STR_SIZE);
        for (int j = 0; j < x; j++) {
            int page_no;
            if ((float)rand() / RAND_MAX < prob) {
                page_no = m_i; // Illegal page number (generates segmentation fault)
            } else {
                page_no = rand() % m_i; // Legal page number [0, m_i - 1]
            }
            char page_no_str[50];
            memset(page_no_str, '\0', 50);
            if (j == x - 1)
                sprintf(page_no_str, "%d", page_no);
            else
                sprintf(page_no_str, "%d,", page_no);
            strcat(ref_str, page_no_str);
        }

        pid_t process_pid = fork();
        if (process_pid == 0) { // Process child
            char MQ1_str[STR_SIZE], MQ3_str[STR_SIZE];
            sprintf(MQ1_str, "%d", MQ1_id);
            sprintf(MQ3_str, "%d", MQ3_id);
            execlp("./process", "./process", MQ1_str, MQ3_str, ref_str, NULL);
            perror("execlp process failed");
            exit(1);
        } else if (process_pid > 0) {
            // Assign PID in parent to eliminate race condition
            page_map[i].pid = process_pid;
        } else {
            perror("fork process failed");
            cleanup_ipc();
            exit(1);
        }
        usleep(250000); // Sleep for 250ms as per project specification
    }

    // Wait until scheduler notifies completion of all processes
    sigset_t block_mask, orig_mask;
    //Empty the signal set of block_mask
    sigemptyset(&block_mask);
    //Add SIGUSR1 to the signal set of block_mask
    sigaddset(&block_mask, SIGUSR1);
    //Temporarily BLOCK any doorbell rings on this list so I can finish an important task without being interrupted
    sigprocmask(SIG_BLOCK, &block_mask, &orig_mask);
    while (waitSched == 1) {
        //Sleep until a doorbell rings, then check if it's my turn to work. If not, go back to sleep.
        sigsuspend(&orig_mask);
    }
    sigprocmask(SIG_UNBLOCK, &block_mask, NULL);

    printf("Scheduler finished...\n");

    kill(sched_pid, SIGINT); // Terminate the scheduler
    usleep(500000);          // Sleep for 500ms to allow MMU to finalize and print result.txt
    kill(mmu_pid, SIGINT);   // Terminate the MMU

    // Reap any remaining child processes
    while (waitpid(-1, NULL, WNOHANG) > 0);

    // Destroy the shared memory and message queues
    cleanup_ipc();

    printf("Master process completed cleanly.\n");
    return 0;
}
