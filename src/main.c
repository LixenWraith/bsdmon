/*
 * main.c - bsdmon: a simple CLI system monitor for FreeBSD and Linux (Ubuntu/Debian)
 *
 * Features:
 *  - CPU usage (average over all cores, %)
 *  - Memory usage (total and used in GB, %)
 *  - Disk usage (of "/" partition, total and used in GB, %)
 *  - Network interface information (name, IPv4 address and mask) excluding localhost.
 *
 * This code minimizes dependencies by using only standard C and OS-native libraries.
 *
 * Compile on Linux: gcc main.c -o bsdmon
 * Compile on FreeBSD: cc main.c -o bsdmon
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>

#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif

#ifdef __linux__
#include <ctype.h>
#endif

// --- CPU usage ---
// We define a structure to hold CPU time counters.
typedef struct {
    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
#ifdef __FreeBSD__
    unsigned long long intr;  // interrupt time (from kern.cp_times)
#endif
} cpu_times_t;

// Function prototypes
int get_cpu_times(cpu_times_t *times);
double calc_cpu_usage(const cpu_times_t *prev, const cpu_times_t *curr);

// On Linux, we parse /proc/stat
#ifdef __linux__
int get_cpu_times(cpu_times_t *times) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        perror("fopen /proc/stat");
        return -1;
    }
    // The first line begins with "cpu " followed by fields.
    char buf[256];
    if (!fgets(buf, sizeof(buf), fp)) {
        perror("fgets");
        fclose(fp);
        return -1;
    }
    fclose(fp);

    // Expected format: cpu  user nice system idle iowait irq softirq steal guest guest_nice
    // We will sum only the first four fields (or five if you want iowait) for simplicity.
    unsigned long long user, nice, system, idle;
    // Optionally include iowait: unsigned long long iowait;
    int ret = sscanf(buf, "cpu  %llu %llu %llu %llu", &user, &nice, &system, &idle);
    if (ret < 4) {
        fprintf(stderr, "Failed to parse /proc/stat cpu line\n");
        return -1;
    }
    times->user = user;
    times->nice = nice;
    times->system = system;
    times->idle = idle;
    return 0;
}
#endif

// On FreeBSD, we use sysctl "kern.cp_times"
// Note: kern.cp_times returns an array of (usually 5) counters per CPU core.
// The order is: CP_USER, CP_NICE, CP_SYS, CP_INTR, CP_IDLE.
#ifdef __FreeBSD__

#ifndef KERN_CP_TIME
#define KERN_CP_TIME 12
#endif

#define CPUSTATES 5

int get_cpu_times(cpu_times_t *times) {
    size_t len;
    int mib[2] = { CTL_KERN, KERN_CP_TIME };

    // First, determine the size of the data
    if (sysctl(mib, 2, NULL, &len, NULL, 0) < 0) {
        perror("sysctl (get size of kern.cp_times)");
        return -1;
    }

    int num_entries = len / sizeof(long);
    int num_cpus = num_entries / CPUSTATES;  // CPUSTATES is usually 5
    long *cp_times = malloc(len);
    if (!cp_times) {
        perror("malloc");
        return -1;
    }

    // Now get the actual CPU times
    if (sysctl(mib, 2, cp_times, &len, NULL, 0) < 0) {
        perror("sysctl (get kern.cp_times)");
        free(cp_times);
        return -1;
    }

    // Aggregate values across all CPU cores
    unsigned long long user = 0, nice = 0, system = 0, intr = 0, idle = 0;
    for (int i = 0; i < num_cpus; i++) {
        user   += cp_times[i * CPUSTATES + 0];
        nice   += cp_times[i * CPUSTATES + 1];
        system += cp_times[i * CPUSTATES + 2];
        intr   += cp_times[i * CPUSTATES + 3];
        idle   += cp_times[i * CPUSTATES + 4];
    }

    // Free the allocated memory
    free(cp_times);

    // Store the aggregated CPU times
    times->user = user;
    times->nice = nice;
    times->system = system;
    times->intr = intr;
    times->idle = idle;

    return 0;
}
#endif

// Compute CPU usage percent between two samples.
double calc_cpu_usage(const cpu_times_t *prev, const cpu_times_t *curr) {
    unsigned long long prev_active, curr_active, prev_total, curr_total;
#ifdef __FreeBSD__
    // On FreeBSD we include intr time in active
    prev_active = prev->user + prev->nice + prev->system + prev->intr;
    curr_active = curr->user + curr->nice + curr->system + curr->intr;
#else
    prev_active = prev->user + prev->nice + prev->system;
    curr_active = curr->user + curr->nice + curr->system;
#endif
    prev_total = prev_active + prev->idle;
    curr_total = curr_active + curr->idle;

    unsigned long long total_delta = curr_total - prev_total;
    unsigned long long active_delta = curr_active - prev_active;
    if (total_delta == 0) return 0.0;
    return ((double)active_delta / total_delta) * 100.0;
}

// --- Memory usage ---
// On Linux: parse /proc/meminfo for MemTotal and MemAvailable.
// On FreeBSD: use sysctl to get hw.physmem and free pages count.
#ifdef __linux__
int get_memory_usage(double *used_gb, double *total_gb, double *percent_used) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        perror("fopen /proc/meminfo");
        return -1;
    }
    char line[256];
    unsigned long mem_total = 0, mem_available = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "MemTotal: %lu kB", &mem_total) == 1)
            continue;
        if (sscanf(line, "MemAvailable: %lu kB", &mem_available) == 1)
            continue;
    }
    fclose(fp);
    if (mem_total == 0) {
        fprintf(stderr, "Failed to get MemTotal\n");
        return -1;
    }
    // Convert kB to GB.
    *total_gb = mem_total / 1024.0 / 1024.0;
    unsigned long mem_used = mem_total - mem_available;
    *used_gb = mem_used / 1024.0 / 1024.0;
    *percent_used = ((double)mem_used / mem_total) * 100.0;
    return 0;
}
#endif

#ifdef __FreeBSD__
#include <sys/param.h>
#include <sys/mount.h>
int get_memory_usage(double *used_gb, double *total_gb, double *percent_used) {
    unsigned long total_mem = 0;
    size_t len = sizeof(total_mem);
    if (sysctlbyname("hw.physmem", &total_mem, &len, NULL, 0) < 0) {
        perror("sysctl hw.physmem");
        return -1;
    }
    // Get page size.
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 0) {
        perror("sysconf _SC_PAGESIZE");
        return -1;
    }
    // Get free pages count.
    unsigned long free_pages = 0;
    len = sizeof(free_pages);
    if (sysctlbyname("vm.stats.vm.v_free_count", &free_pages, &len, NULL, 0) < 0) {
        perror("sysctl vm.stats.vm.v_free_count");
        return -1;
    }
    unsigned long free_mem = free_pages * page_size;
    unsigned long used_mem = total_mem > free_mem ? total_mem - free_mem : 0;
    *total_gb = total_mem / 1024.0 / 1024.0 / 1024.0;
    *used_gb = used_mem / 1024.0 / 1024.0 / 1024.0;
    *percent_used = ((double)used_mem / total_mem) * 100.0;
    return 0;
}
#endif

// --- Disk usage ---
// We use statvfs on the "/" mount point.
int get_disk_usage(double *used_gb, double *total_gb, double *percent_used) {
    struct statvfs vfs;
    if (statvfs("/", &vfs) < 0) {
        perror("statvfs");
        return -1;
    }
    unsigned long long total = vfs.f_blocks * vfs.f_frsize;
    unsigned long long free = vfs.f_bfree * vfs.f_frsize;
    unsigned long long used = total - free;
    *total_gb = total / 1024.0 / 1024.0 / 1024.0;
    *used_gb = used / 1024.0 / 1024.0 / 1024.0;
    *percent_used = total ? ((double)used / total) * 100.0 : 0.0;
    return 0;
}

// --- Network interfaces ---
// Use getifaddrs to list interfaces with an IPv4 address,
// ignoring the loopback interface.
void print_network_interfaces(void) {
    struct ifaddrs *ifaddr, *ifa;
    // Get list of network interfaces
    if (getifaddrs(&ifaddr) < 0) {
        perror("getifaddrs");
        return;
    }
    printf("Network interfaces:\n");
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        // We only want interfaces with an address.
        if (!ifa->ifa_addr)
            continue;
        if (ifa->ifa_addr->sa_family != AF_INET)
            continue;
        // Filter out loopback (by flag or name)
        if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;
        // (Alternatively, filter by name: if (strcmp(ifa->ifa_name, "lo") == 0) continue;)
        char ip[INET_ADDRSTRLEN];
        char netmask[INET_ADDRSTRLEN];
        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
        struct sockaddr_in *mask = (struct sockaddr_in *)ifa->ifa_netmask;
        if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)))
            continue;
        if (!inet_ntop(AF_INET, &mask->sin_addr, netmask, sizeof(netmask)))
            continue;
        printf("  %s: %s (mask: %s)\n", ifa->ifa_name, ip, netmask);
    }
    freeifaddrs(ifaddr);
}

int main(void) {
    printf("bsdmon - System Monitor\n");
    printf("=======================\n");

    // CPU usage: take two samples one second apart.
    cpu_times_t prev, curr;
    if (get_cpu_times(&prev) != 0) {
        fprintf(stderr, "Failed to get initial CPU times\n");
        return EXIT_FAILURE;
    }
    sleep(1);
    if (get_cpu_times(&curr) != 0) {
        fprintf(stderr, "Failed to get CPU times\n");
        return EXIT_FAILURE;
    }
    double cpu_usage = calc_cpu_usage(&prev, &curr);
    printf("CPU Usage: %.2f%%\n", cpu_usage);

    // Memory usage
    double mem_used_gb, mem_total_gb, mem_percent;
    if (get_memory_usage(&mem_used_gb, &mem_total_gb, &mem_percent) == 0) {
        printf("Memory Usage: %.2f GB / %.2f GB (%.2f%% used)\n",
               mem_used_gb, mem_total_gb, mem_percent);
    } else {
        printf("Memory Usage: Error retrieving information\n");
    }

    // Disk usage
    double disk_used_gb, disk_total_gb, disk_percent;
    if (get_disk_usage(&disk_used_gb, &disk_total_gb, &disk_percent) == 0) {
        printf("Disk Usage (\"/\"): %.2f GB / %.2f GB (%.2f%% used)\n",
               disk_used_gb, disk_total_gb, disk_percent);
    } else {
        printf("Disk Usage: Error retrieving information\n");
    }

    // Network interfaces
    print_network_interfaces();

    return EXIT_SUCCESS;
}
