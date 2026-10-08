/*
 * SMART WORKLOAD MANAGER
 *
 * Features:
 * 1. Select Primary Workload
 * 2. View Running Processes
 * 3. Analyze Specific Process
 * 4. Analyze Primary Workload
 * 5. Detect Workload Imbalance
 * 6. Balance Background Workload
 * 7. Restore Process Priority
 * 8. System Workload Status
 * 9. View Activity Log
 * 10. Exit
 *
 * Linux / WSL
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <errno.h>
#include <time.h>

#define MAX_PROCESSES 512
#define NAME_SIZE 128
#define LOG_SIZE 512

typedef struct {
    int pid;
    char name[NAME_SIZE];
    long memory_kb;
    long priority;
    double cpu_usage;
    char state;
} ProcessInfo;

typedef struct {
    int pid;
    int original_priority;
    int changed;
} PriorityRecord;

int primary_pid = -1;

PriorityRecord priority_records[MAX_PROCESSES];
int priority_count = 0;


/* ---------------------------------------------------------
   LOGGING
   --------------------------------------------------------- */

void write_log(const char *message)
{
    FILE *fp;
    time_t now;
    struct tm *t;
    char time_string[64];

    fp = fopen("activity_log.txt", "a");

    if (fp == NULL) {
        return;
    }

    now = time(NULL);
    t = localtime(&now);

    if (t != NULL) {
        strftime(time_string, sizeof(time_string),
                 "%Y-%m-%d %H:%M:%S", t);

        fprintf(fp, "[%s] %s\n", time_string, message);
    }

    fclose(fp);
}


/* ---------------------------------------------------------
   PROCESS NAME
   --------------------------------------------------------- */

int get_process_name(int pid, char *name, size_t size)
{
    char path[64];
    FILE *fp;

    snprintf(path, sizeof(path), "/proc/%d/comm", pid);

    fp = fopen(path, "r");

    if (fp == NULL) {
        return 0;
    }

    if (fgets(name, size, fp) == NULL) {
        fclose(fp);
        return 0;
    }

    name[strcspn(name, "\n")] = '\0';

    fclose(fp);

    return 1;
}


/* ---------------------------------------------------------
   MEMORY USAGE
   --------------------------------------------------------- */

long get_memory_usage(int pid)
{
    char path[64];
    char line[256];
    FILE *fp;
    long memory = 0;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);

    fp = fopen(path, "r");

    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {

        if (strncmp(line, "VmRSS:", 6) == 0) {

            sscanf(line + 6, "%ld", &memory);

            break;
        }
    }

    fclose(fp);

    return memory;
}


/* ---------------------------------------------------------
   PROCESS STATE
   --------------------------------------------------------- */

char get_process_state(int pid)
{
    char path[64];
    char line[512];
    FILE *fp;
    char state = '?';

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    fp = fopen(path, "r");

    if (fp == NULL) {
        return '?';
    }

    if (fgets(line, sizeof(line), fp) != NULL) {

        char *right_paren = strrchr(line, ')');

        if (right_paren != NULL &&
            right_paren[1] == ' ') {

            state = right_paren[2];
        }
    }

    fclose(fp);

    return state;
}


/* ---------------------------------------------------------
   PROCESS PRIORITY
   --------------------------------------------------------- */

long get_process_priority(int pid)
{
    int result;

    errno = 0;

    result = getpriority(PRIO_PROCESS, pid);

    if (errno != 0) {
        return -1000;
    }

    return result;
}


/* ---------------------------------------------------------
   PROCESS CPU TIME
   --------------------------------------------------------- */

unsigned long long get_process_jiffies(int pid)
{
    char path[64];
    char line[2048];
    FILE *fp;

    unsigned long long utime = 0;
    unsigned long long stime = 0;

    char *right_paren;
    char *ptr;

    int field = 3;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    fp = fopen(path, "r");

    if (fp == NULL) {
        return 0;
    }

    if (fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return 0;
    }

    fclose(fp);

    /*
     * Process name can contain spaces.
     * Therefore find the final ')' first.
     */

    right_paren = strrchr(line, ')');

    if (right_paren == NULL) {
        return 0;
    }

    ptr = right_paren + 2;

    /*
     * ptr now starts at field 3: state
     */

    field = 3;

    while (*ptr != '\0' && field <= 15) {

        char *end;
        char token[64];
        size_t len;

        while (*ptr == ' ') {
            ptr++;
        }

        if (*ptr == '\0') {
            break;
        }

        end = ptr;

        while (*end != '\0' && *end != ' ') {
            end++;
        }

        len = (size_t)(end - ptr);

        if (len >= sizeof(token)) {
            len = sizeof(token) - 1;
        }

        memcpy(token, ptr, len);
        token[len] = '\0';

        if (field == 14) {
            utime = strtoull(token, NULL, 10);
        }

        if (field == 15) {
            stime = strtoull(token, NULL, 10);
        }

        ptr = end;

        field++;
    }

    return utime + stime;
}


/* ---------------------------------------------------------
   TOTAL CPU JIFFIES
   --------------------------------------------------------- */

unsigned long long get_total_cpu_jiffies(void)
{
    FILE *fp;
    char line[2048];

    unsigned long long total = 0;
    unsigned long long value;

    fp = fopen("/proc/stat", "r");

    if (fp == NULL) {
        return 0;
    }

    if (fgets(line, sizeof(line), fp) != NULL) {

        if (strncmp(line, "cpu ", 4) == 0) {

            char *ptr = line + 4;

            while (sscanf(ptr, "%llu", &value) == 1) {

                total += value;

                while (*ptr != '\0' &&
                       !isspace((unsigned char)*ptr)) {
                    ptr++;
                }

                while (isspace((unsigned char)*ptr)) {
                    ptr++;
                }
            }
        }
    }

    fclose(fp);

    return total;
}


/* ---------------------------------------------------------
   CPU USAGE
   --------------------------------------------------------- */

double calculate_cpu_usage(int pid)
{
    unsigned long long process_start;
    unsigned long long total_start;

    unsigned long long process_end;
    unsigned long long total_end;

    unsigned long long process_diff;
    unsigned long long total_diff;

    double cpu;

    process_start = get_process_jiffies(pid);
    total_start = get_total_cpu_jiffies();

    if (process_start == 0 || total_start == 0) {
        return 0.0;
    }

    usleep(500000);

    process_end = get_process_jiffies(pid);
    total_end = get_total_cpu_jiffies();

    if (process_end == 0 || total_end == 0) {
        return 0.0;
    }

    process_diff = process_end - process_start;
    total_diff = total_end - total_start;

    if (total_diff == 0) {
        return 0.0;
    }

    cpu = ((double)process_diff /
           (double)total_diff) * 100.0;

    /*
     * On multicore systems, one fully busy core may appear
     * as approximately 100 / number_of_CPUs.
     *
     * Multiply by CPU count so that one fully used core
     * is shown as approximately 100%.
     */

    long cpus = sysconf(_SC_NPROCESSORS_ONLN);

    if (cpus > 0) {
        cpu *= cpus;
    }

    if (cpu > 100.0) {
        cpu = 100.0;
    }

    if (cpu < 0.0) {
        cpu = 0.0;
    }

    return cpu;
}


/* ---------------------------------------------------------
   PROCESS INFORMATION
   --------------------------------------------------------- */

int get_process_info(int pid, ProcessInfo *p)
{
    if (!get_process_name(pid, p->name,
                          sizeof(p->name))) {
        return 0;
    }

    p->pid = pid;

    p->memory_kb = get_memory_usage(pid);

    p->priority = get_process_priority(pid);

    p->state = get_process_state(pid);

    p->cpu_usage = calculate_cpu_usage(pid);

    return 1;
}


/* ---------------------------------------------------------
   WORKLOAD SCORE
   --------------------------------------------------------- */

int calculate_workload_score(double cpu, long memory)
{
    int score = 0;

    /*
     * CPU contribution: maximum 70 points
     */

    score += (int)(cpu * 0.7);

    /*
     * Memory contribution: maximum 30 points.
     * 100 MB or more gives full memory score.
     */

    if (memory > 100000) {
        score += 30;
    }
    else if (memory > 50000) {
        score += 20;
    }
    else if (memory > 20000) {
        score += 10;
    }

    if (score > 100) {
        score = 100;
    }

    return score;
}


/* ---------------------------------------------------------
   WORKLOAD LEVEL
   --------------------------------------------------------- */

const char *get_workload_level(int score)
{
    if (score >= 80) {
        return "CRITICAL";
    }

    if (score >= 60) {
        return "HIGH";
    }

    if (score >= 30) {
        return "MEDIUM";
    }

    return "LOW";
}


/* ---------------------------------------------------------
   DISPLAY PROCESS ANALYSIS
   --------------------------------------------------------- */

void display_analysis(ProcessInfo *p, int is_primary)
{
    int score;

    score = calculate_workload_score(
        p->cpu_usage,
        p->memory_kb
    );

    printf("\n");
    printf("==============================================\n");
    printf("          SMART WORKLOAD ANALYSIS\n");
    printf("==============================================\n");

    printf("Process Name       : %s\n", p->name);
    printf("PID                : %d\n", p->pid);
    printf("Process State      : %c\n", p->state);
    printf("CPU Usage          : %.2f%%\n", p->cpu_usage);
    printf("Memory Usage       : %ld KB\n", p->memory_kb);
    printf("Current Priority   : %ld\n", p->priority);

    printf("----------------------------------------------\n");

    printf("Workload Score     : %d / 100\n", score);
    printf("Workload Level     : %s\n",
           get_workload_level(score));

    if (is_primary) {
        printf("Workload Role      : PRIMARY WORKLOAD\n");
    }
    else {
        printf("Workload Role      : BACKGROUND WORKLOAD\n");
    }

    printf("----------------------------------------------\n");

    if (is_primary) {
        printf("Protection Status  : IMPORTANT PROCESS\n");
        printf("Recommendation     : Protect this workload\n");
    }
    else if (p->cpu_usage >= 50.0) {
        printf("Protection Status  : RESOURCE INTENSIVE\n");
        printf("Recommendation     : Consider lowering priority\n");
    }
    else {
        printf("Protection Status  : NORMAL\n");
        printf("Recommendation     : No action required\n");
    }

    printf("==============================================\n");
}


/* ---------------------------------------------------------
   OPTION 1
   SELECT PRIMARY WORKLOAD
   --------------------------------------------------------- */

void select_primary_workload(void)
{
    int pid;
    char name[NAME_SIZE];
    char log_message[LOG_SIZE];

    printf("\n");
    printf("==============================================\n");
    printf("          SELECT PRIMARY WORKLOAD\n");
    printf("==============================================\n");

    printf("Enter PID of the important/running program: ");
    scanf("%d", &pid);

    if (get_process_name(pid, name, sizeof(name))) {

        primary_pid = pid;

        printf("\nPrimary workload selected successfully.\n");
        printf("Process : %s\n", name);
        printf("PID     : %d\n", primary_pid);

        printf("\nThe system will now protect this workload\n");
        printf("when workload contention is detected.\n");

        snprintf(log_message, sizeof(log_message),
                 "Primary workload selected: %s (PID %d)",
                 name, pid);

        write_log(log_message);
    }
    else {

        printf("\nInvalid PID or process does not exist.\n");
    }
}


/* ---------------------------------------------------------
   OPTION 2
   VIEW RUNNING PROCESSES
   --------------------------------------------------------- */

void view_running_processes(void)
{
    DIR *dir;
    struct dirent *entry;

    int count = 0;

    dir = opendir("/proc");

    if (dir == NULL) {
        printf("Unable to access /proc.\n");
        return;
    }

    printf("\n");
    printf("============================================================\n");
    printf("                  RUNNING PROCESSES\n");
    printf("============================================================\n");

    printf("%-8s %-25s %-8s %-8s %-8s\n",
           "PID", "PROCESS", "CPU%", "MEM(KB)", "NI");

    printf("------------------------------------------------------------\n");

    while ((entry = readdir(dir)) != NULL) {

        int pid;
        char name[NAME_SIZE];
        long memory;
        long priority;

        if (!isdigit((unsigned char)entry->d_name[0])) {
            continue;
        }

        pid = atoi(entry->d_name);

        if (pid <= 0) {
            continue;
        }

        if (!get_process_name(pid, name, sizeof(name))) {
            continue;
        }

        memory = get_memory_usage(pid);
        priority = get_process_priority(pid);

        printf("%-8d %-25s %-8s %-8ld %-8ld",
               pid,
               name,
               "N/A",
               memory,
               priority);

        if (pid == primary_pid) {
            printf("  <-- PRIMARY");
        }

        printf("\n");

        count++;

        if (count >= 50) {
            break;
        }
    }

    closedir(dir);

    printf("============================================================\n");
}


/* ---------------------------------------------------------
   OPTION 3
   ANALYZE SPECIFIC PROCESS
   --------------------------------------------------------- */

void analyze_specific_process(void)
{
    int pid;
    ProcessInfo p;

    printf("\n");
    printf("==============================================\n");
    printf("          ANALYZE SPECIFIC PROCESS\n");
    printf("==============================================\n");

    printf("Enter PID: ");
    scanf("%d", &pid);

    if (!get_process_info(pid, &p)) {

        printf("\nProcess not found or access denied.\n");
        return;
    }

    display_analysis(&p, pid == primary_pid);
}


/* ---------------------------------------------------------
   OPTION 4
   ANALYZE PRIMARY WORKLOAD
   --------------------------------------------------------- */

void analyze_primary_workload(void)
{
    ProcessInfo p;

    if (primary_pid <= 0) {

        printf("\nNo primary workload selected.\n");
        printf("Use Option 1 first.\n");

        return;
    }

    if (!get_process_info(primary_pid, &p)) {

        printf("\nPrimary workload is no longer running.\n");

        return;
    }

    display_analysis(&p, 1);
}


/* ---------------------------------------------------------
   CHECK WHETHER PID IS PRIMARY
   --------------------------------------------------------- */

int is_primary_process(int pid)
{
    return pid == primary_pid;
}


/* ---------------------------------------------------------
   OPTION 5
   DETECT WORKLOAD IMBALANCE
   --------------------------------------------------------- */

void detect_workload_imbalance(void)
{
    DIR *dir;
    struct dirent *entry;

    int background_count = 0;
    int heavy_count = 0;

    double background_cpu = 0.0;

    int heavy_pid = -1;
    double heavy_cpu = 0.0;

    char heavy_name[NAME_SIZE] = "";

    printf("\n");
    printf("==============================================\n");
    printf("          WORKLOAD IMBALANCE DETECTION\n");
    printf("==============================================\n");

    if (primary_pid <= 0) {

        printf("\nNo primary workload selected.\n");
        printf("Use Option 1 first.\n");

        return;
    }

    if (!get_process_name(primary_pid,
                          heavy_name,
                          sizeof(heavy_name))) {

        printf("\nPrimary process is no longer running.\n");
        return;
    }

    printf("\nPrimary Workload:\n");
    printf("PID        : %d\n", primary_pid);
    printf("Name       : %s\n",
           heavy_name);

    /*
     * Scan /proc
     */

    dir = opendir("/proc");

    if (dir == NULL) {
        printf("\nUnable to access /proc.\n");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {

        int pid;
        char name[NAME_SIZE];
        long memory;
        long priority;
        double cpu;

        if (!isdigit((unsigned char)entry->d_name[0])) {
            continue;
        }

        pid = atoi(entry->d_name);

        if (pid <= 0) {
            continue;
        }

        if (pid == primary_pid) {
            continue;
        }

        if (!get_process_name(pid, name, sizeof(name))) {
            continue;
        }

        memory = get_memory_usage(pid);
        priority = get_process_priority(pid);

        if (memory < 0 || priority == -1000) {
            continue;
        }

        cpu = calculate_cpu_usage(pid);

        background_count++;
        background_cpu += cpu;

        /*
         * A process using >= 50% CPU is considered
         * a heavy background task.
         */

        if (cpu >= 50.0) {

            heavy_count++;

            if (cpu > heavy_cpu) {

                heavy_cpu = cpu;
                heavy_pid = pid;

                strncpy(heavy_name,
                        name,
                        sizeof(heavy_name) - 1);

                heavy_name[sizeof(heavy_name) - 1] = '\0';
            }
        }

        /*
         * Avoid scanning too many processes during demo.
         */

        if (background_count >= 40) {
            break;
        }
    }

    closedir(dir);

    printf("\nBackground Processes     : %d\n",
           background_count);

    printf("Background CPU Load      : %.2f%%\n",
           background_cpu);

    printf("Heavy Background Tasks   : %d\n",
           heavy_count);

    printf("----------------------------------------------\n");

    if (heavy_count > 0) {

        printf("STATUS: WORKLOAD IMBALANCE DETECTED\n");

        printf("\nHeavy Background Process:\n");
        printf("Name       : %s\n", heavy_name);
        printf("PID        : %d\n", heavy_pid);
        printf("CPU Usage  : %.2f%%\n", heavy_cpu);

        printf("\nRecommendation:\n");
        printf("Lower the priority of the heavy background\n");
        printf("process to protect the primary workload.\n");

        write_log("Workload imbalance detected.");
    }
    else {

        printf("STATUS: WORKLOAD BALANCED\n");
        printf("No significant workload contention detected.\n");
    }
}


/* ---------------------------------------------------------
   FIND PRIORITY RECORD
   --------------------------------------------------------- */

int find_priority_record(int pid)
{
    int i;

    for (i = 0; i < priority_count; i++) {

        if (priority_records[i].pid == pid) {
            return i;
        }
    }

    return -1;
}


/* ---------------------------------------------------------
   OPTION 6
   BALANCE BACKGROUND WORKLOAD
   --------------------------------------------------------- */

void balance_background_workload(void)
{
    int pid;
    int old_priority;
    int new_priority;

    char name[NAME_SIZE];
    char log_message[LOG_SIZE];

    printf("\n");
    printf("==============================================\n");
    printf("          BALANCE BACKGROUND WORKLOAD\n");
    printf("==============================================\n");

    if (primary_pid <= 0) {

        printf("\nSelect a primary workload first using Option 1.\n");
        return;
    }

    printf("Enter PID of background process: ");
    scanf("%d", &pid);

    if (pid == primary_pid) {

        printf("\nERROR: Primary workload cannot be changed.\n");
        return;
    }

    if (!get_process_name(pid, name, sizeof(name))) {

        printf("\nProcess not found.\n");
        return;
    }

    old_priority = (int)get_process_priority(pid);

    if (old_priority == -1000) {

        printf("\nUnable to read process priority.\n");
        return;
    }

    /*
     * Store original priority only once.
     */

    if (find_priority_record(pid) == -1) {

        if (priority_count < MAX_PROCESSES) {

            priority_records[priority_count].pid = pid;
            priority_records[priority_count].original_priority =
                old_priority;
            priority_records[priority_count].changed = 1;

            priority_count++;
        }
    }

    /*
     * Increase nice value.
     * Higher nice value = lower CPU scheduling priority.
     */

    new_priority = old_priority + 5;

    if (new_priority > 19) {
        new_priority = 19;
    }

    if (setpriority(PRIO_PROCESS, pid, new_priority) != 0) {

        perror("\nUnable to change process priority");

        printf("You may not have permission to modify this process.\n");

        return;
    }

    printf("\nBackground workload balanced successfully.\n");

    printf("Process          : %s\n", name);
    printf("PID              : %d\n", pid);
    printf("Old Priority     : %d\n", old_priority);
    printf("New Priority     : %d\n", new_priority);

    printf("\nPrimary workload remains protected.\n");

    snprintf(log_message, sizeof(log_message),
             "Background priority changed: %s (PID %d), %d -> %d",
             name,
             pid,
             old_priority,
             new_priority);

    write_log(log_message);
}


/* ---------------------------------------------------------
   OPTION 7
   RESTORE PRIORITY
   --------------------------------------------------------- */

void restore_process_priority(void)
{
    int pid;
    int index;
    int original_priority;

    char name[NAME_SIZE];
    char log_message[LOG_SIZE];

    printf("\n");
    printf("==============================================\n");
    printf("          RESTORE PROCESS PRIORITY\n");
    printf("==============================================\n");

    printf("Enter PID: ");
    scanf("%d", &pid);

    if (!get_process_name(pid, name, sizeof(name))) {

        printf("\nProcess not found.\n");
        return;
    }

    index = find_priority_record(pid);

    if (index == -1) {

        printf("\nNo previous priority change was recorded\n");
        printf("for this process.\n");

        return;
    }

    original_priority =
        priority_records[index].original_priority;

    if (setpriority(PRIO_PROCESS,
                    pid,
                    original_priority) != 0) {

        perror("\nUnable to restore process priority");
        return;
    }

    printf("\nPriority restored successfully.\n");

    printf("Process          : %s\n", name);
    printf("PID              : %d\n", pid);
    printf("Restored Priority: %d\n",
           original_priority);

    snprintf(log_message, sizeof(log_message),
             "Process priority restored: %s (PID %d)",
             name,
             pid);

    write_log(log_message);
}


/* ---------------------------------------------------------
   TOTAL MEMORY
   --------------------------------------------------------- */

long get_total_memory_kb(void)
{
    FILE *fp;
    char line[256];

    long total = 0;

    fp = fopen("/proc/meminfo", "r");

    if (fp == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {

        if (strncmp(line, "MemTotal:", 9) == 0) {

            sscanf(line + 9, "%ld", &total);

            break;
        }
    }

    fclose(fp);

    return total;
}


/* ---------------------------------------------------------
   AVAILABLE MEMORY
   --------------------------------------------------------- */

long get_available_memory_kb(void)
{
    FILE *fp;
    char line[256];

    long available = 0;

    fp = fopen("/proc/meminfo", "r");

    if (fp == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {

        if (strncmp(line, "MemAvailable:", 13) == 0) {

            sscanf(line + 13, "%ld", &available);

            break;
        }
    }

    fclose(fp);

    return available;
}


/* ---------------------------------------------------------
   OPTION 8
   SYSTEM WORKLOAD STATUS
   --------------------------------------------------------- */

void system_workload_status(void)
{
    long total_memory;
    long available_memory;
    long used_memory;

    double memory_percent;

    printf("\n");
    printf("==============================================\n");
    printf("             SYSTEM WORKLOAD STATUS\n");
    printf("==============================================\n");

    total_memory = get_total_memory_kb();
    available_memory = get_available_memory_kb();

    if (total_memory > 0) {

        used_memory =
            total_memory - available_memory;

        memory_percent =
            ((double)used_memory /
             (double)total_memory) * 100.0;

        printf("Total Memory       : %ld MB\n",
               total_memory / 1024);

        printf("Used Memory        : %ld MB\n",
               used_memory / 1024);

        printf("Available Memory   : %ld MB\n",
               available_memory / 1024);

        printf("Memory Utilization : %.2f%%\n",
               memory_percent);
    }
    else {

        printf("Memory information unavailable.\n");
    }

    printf("----------------------------------------------\n");

    if (primary_pid > 0) {

        char name[NAME_SIZE];

        if (get_process_name(primary_pid,
                             name,
                             sizeof(name))) {

            printf("Primary Workload   : %s\n",
                   name);

            printf("Primary PID        : %d\n",
                   primary_pid);

            printf("Protection         : ENABLED\n");
        }
        else {

            printf("Primary Workload   : Not Running\n");
        }
    }
    else {

        printf("Primary Workload   : Not Selected\n");
    }

    printf("==============================================\n");
}


/* ---------------------------------------------------------
   OPTION 9
   VIEW ACTIVITY LOG
   --------------------------------------------------------- */

void view_activity_log(void)
{
    FILE *fp;
    char line[LOG_SIZE];

    printf("\n");
    printf("==============================================\n");
    printf("              ACTIVITY LOG\n");
    printf("==============================================\n");

    fp = fopen("activity_log.txt", "r");

    if (fp == NULL) {

        printf("\nNo activity log available yet.\n");
        return;
    }

    while (fgets(line, sizeof(line), fp)) {

        printf("%s", line);
    }

    fclose(fp);

    printf("==============================================\n");
}


/* ---------------------------------------------------------
   MENU
   --------------------------------------------------------- */

void display_menu(void)
{
    printf("\n\n");
    printf("====================================================\n");
    printf("              SMART WORKLOAD MANAGER\n");
    printf("====================================================\n");

    printf("1. Select Primary Workload\n");
    printf("2. View Running Processes\n");
    printf("3. Analyze Specific Process\n");
    printf("4. Analyze Primary Workload\n");
    printf("5. Detect Workload Imbalance\n");
    printf("6. Balance Background Workload\n");
    printf("7. Restore Process Priority\n");
    printf("8. System Workload Status\n");
    printf("9. View Activity Log\n");
    printf("10. Exit\n");

    printf("====================================================\n");
}


/* ---------------------------------------------------------
   MAIN
   --------------------------------------------------------- */

int main(void)
{
    int choice;

    printf("\n");
    printf("====================================================\n");
    printf("          SMART WORKLOAD MANAGER STARTED\n");
    printf("====================================================\n");

    printf("\nMonitoring Linux processes and system resources...\n");

    write_log("Smart Workload Manager started.");

    while (1) {

        display_menu();

        printf("Enter your choice: ");
        scanf("%d", &choice);

        switch (choice) {

            case 1:
                select_primary_workload();
                break;

            case 2:
                view_running_processes();
                break;

            case 3:
                analyze_specific_process();
                break;

            case 4:
                analyze_primary_workload();
                break;

            case 5:
                detect_workload_imbalance();
                break;

            case 6:
                balance_background_workload();
                break;

            case 7:
                restore_process_priority();
                break;

            case 8:
                system_workload_status();
                break;

            case 9:
                view_activity_log();
                break;

            case 10:

                write_log("Smart Workload Manager exited.");

                printf("\nExiting Smart Workload Manager...\n");
                printf("Thank you.\n");

                return 0;

            default:

                printf("\nInvalid choice. Please select 1-10.\n");
        }
    }

    return 0;
}
