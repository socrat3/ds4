/* ds4_media_mem - quanta memoria tiene gia' ComfyUI. Vedi ds4_media_int.h.
 *
 * Il gate conta come disponibile anche la memoria che ComfyUI ha gia' preso: la riusa
 * per il lavoro dopo. torch_vram_total di /system_stats non basta: con l'allocatore
 * cudaMallocAsync (quello di ComfyUI sul DGX Spark) resta sotto 1 GiB anche con 50 GiB
 * di pesi H3 caricati, e ogni secondo lavoro veniva rifiutato. La misura vera e' la
 * memoria GPU del processo, da nvidia-smi; il processo e' quello in ascolto sulla
 * porta di ComfyUI, trovato in /proc (solo per un ComfyUI su questa macchina). */
#include "ds4_media_int.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* inode del socket in ascolto sulla porta, da /proc/net/tcp o tcp6; 0 se non c'e'. */
static unsigned long mem_listen_inode(int port) {
    const char *files[] = {"/proc/net/tcp", "/proc/net/tcp6"};
    for (int f = 0; f < 2; f++) {
        FILE *fp = fopen(files[f], "r");
        if (!fp) continue;
        char l[512];
        unsigned long inode = 0;
        if (!fgets(l, sizeof(l), fp)) { fclose(fp); continue; }   /* intestazione */
        while (!inode && fgets(l, sizeof(l), fp)) {
            char local[128], remote[128];
            unsigned st, lport;
            unsigned long ino;
            /* sl local rem st tx:rx tr:when retrnsmt uid timeout inode */
            if (sscanf(l, "%*s %127s %127s %x %*s %*s %*s %*u %*u %lu", local, remote, &st, &ino) != 4) continue;
            char *colon = strrchr(local, ':');
            if (!colon || sscanf(colon + 1, "%x", &lport) != 1) continue;
            if (st == 0x0A && (int)lport == port && ino) inode = ino;   /* 0A = LISTEN */
        }
        fclose(fp);
        if (inode) return inode;
    }
    return 0;
}

long media_pid_on_port(int port) {
    unsigned long inode = mem_listen_inode(port);
    if (!inode) return -1;
    char want[64];
    snprintf(want, sizeof(want), "socket:[%lu]", inode);
    DIR *proc = opendir("/proc");
    if (!proc) return -1;
    long found = -1;
    struct dirent *e;
    while (found < 0 && (e = readdir(proc))) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0) continue;
        char fdp[64];
        snprintf(fdp, sizeof(fdp), "/proc/%ld/fd", pid);
        DIR *fds = opendir(fdp);   /* processi di altri utenti: non leggibili, si saltano */
        if (!fds) continue;
        struct dirent *d;
        while (found < 0 && (d = readdir(fds))) {
            char lp[400], target[64];
            snprintf(lp, sizeof(lp), "%s/%s", fdp, d->d_name);
            ssize_t n = readlink(lp, target, sizeof(target) - 1);
            if (n > 0) { target[n] = '\0'; if (!strcmp(target, want)) found = pid; }
        }
        closedir(fds);
    }
    closedir(proc);
    return found;
}

long media_gpu_used_kib(long pid) {
    if (pid <= 0) return 0;
    /* comando fisso, nessun dato esterno nella riga: popen va bene */
    FILE *fp = popen("nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits 2>/dev/null", "r");
    if (!fp) return 0;
    char l[256];
    long kib = 0;
    while (fgets(l, sizeof(l), fp)) {
        long p, mib;
        if (sscanf(l, "%ld, %ld", &p, &mib) == 2 && p == pid && mib > 0) kib += mib * 1024;
    }
    pclose(fp);
    return kib;
}
