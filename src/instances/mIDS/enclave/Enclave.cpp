
#include <stdarg.h>
#include <stdio.h> /* vsnprintf */

#include "Enclave.h"
#include "mids_edge_t.h" /* print_string */

#include "../../../lb/networking/libpcap/enclave/pcap_t.h"
extern "C" {
#include "../../../lb/core/enclave/include/etap_t.h"
}

/*
* printf:
*   Invokes OCALL to display the enclave buffer to the terminal.
*/
void printf(const char* fmt, ...)
{
    char buf[BUFSIZ] = { '\0' };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, BUFSIZ, fmt, ap);
    va_end(ap);
    ocall_print_string2(buf);
}

void getTime(struct etime* time)
{
#ifdef USE_ETAP
    // time_t etap_time;
    // get_clock(&etap_time);
    // time->s = etap_time;
    // time->ns = 0;
    struct timeval etap_time;
    get_clock(&etap_time);
    time->s = etap_time.tv_sec;
    time->ns = etap_time.tv_usec;
#else
    ocall_get_time2((int*)&time->s, (int*)&time->ns);
#endif
}

int diffTime(const struct etime* start, const struct etime* end)
{
    //int diff = (end->s - start->s) * 1000000 + (end->ns - start->ns) / 1000;
    //if (diff < 0)
    //{
    //	printf("time error diff%d, %lld:%lld, %lld:%lld.\n", diff, start->s, start->ns, end->s, end->ns);
    //}

    return (end->s - start->s) * 1000000 + (end->ns - start->ns) / 1000;
}
