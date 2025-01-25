#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

struct vmstat_data {
    unsigned long procs_running;
    unsigned long procs_blocked;
    unsigned long mem_swpd;
    unsigned long mem_free;
    unsigned long mem_buff;
    unsigned long mem_cache;
    unsigned long swap_si;
    unsigned long swap_so;
    unsigned long io_bi;
    unsigned long io_bo;
    unsigned long system_in;
    unsigned long system_cs;
    unsigned long cpu_us;
    unsigned long cpu_sy;
    unsigned long cpu_id;
    unsigned long cpu_wa;
    unsigned long cpu_st;
};

#define VMSTAT_GET_DATA _IOR('v', 1, struct vmstat_data)

int main() {
    int fd;
    struct vmstat_data data;

    fd = open("/dev/vmstat_dev", O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return -1;
    }

    if (ioctl(fd, VMSTAT_GET_DATA, &data) < 0) {
        perror("Failed to get vmstat data");
        close(fd);
        return -1;
    }

    printf("procs -----------memory---------- ---swap-- -----io---- --system-- -----cpu------\n");
    printf(" r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs  us sy id wa st\n");
    printf("%2lu %2lu %6lu %6lu %6lu %6lu %4lu %4lu %5lu %5lu %4lu %4lu %2lu %2lu %2lu %2lu %2lu\n",
           data.procs_running,
           data.procs_blocked,
           data.mem_swpd,
           data.mem_free,
           data.mem_buff,
           data.mem_cache,
           data.swap_si,
           data.swap_so,
           data.io_bi,
           data.io_bo,
           data.system_in,
           data.system_cs,
           data.cpu_us,
           data.cpu_sy,
           data.cpu_id,
           data.cpu_wa,
           data.cpu_st);

    close(fd);
    return 0;
}
