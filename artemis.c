#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <dirent.h>

#ifdef __linux__
#include <inttypes.h>
#include <linux/fs.h>
#endif

#define APP_NAME    "Artemis"
#define APP_VERSION "2.0.0-GOLD"

#define BUF_SIZE        (4 * 1024 * 1024)
#define ISO_MAGIC_OFF   0x8001
#define ISO_MAGIC       "CD001"
#define ISO_MAGIC_LEN   5
#define PROGRESS_WIDTH  40

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_DIM    "\033[2m"
#define C_RED    "\033[31m"
#define C_GREEN  "\033[32m"
#define C_YELLOW "\033[33m"
#define C_BLUE   "\033[34m"
#define C_MAGENTA "\033[35m"
#define C_CYAN   "\033[36m"
#define C_GOLD   "\033[38;5;220m"
#define C_SILVER "\033[38;5;246m"
#define C_SKY    "\033[38;5;117m"

struct partition {
    uint8_t  bootable;
    uint8_t  start_head;
    uint8_t  start_sec;
    uint8_t  start_cyl;
    uint8_t  type;
    uint8_t  end_head;
    uint8_t  end_sec;
    uint8_t  end_cyl;
    uint32_t lba_start;
    uint32_t lba_len;
} __attribute__((packed));

struct mbr {
    uint8_t  code[446];
    struct   partition parts[4];
    uint16_t signature;
} __attribute__((packed));

static void die(const char *msg)
{
    fprintf(stderr, C_RED "\nerror: " C_RESET "%s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

static void die_msg(const char *msg)
{
    fprintf(stderr, C_RED "\nerror: " C_RESET "%s\n", msg);
    exit(EXIT_FAILURE);
}

static void fmt_size(uint64_t bytes, char *out, size_t outsz)
{
    const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    snprintf(out, outsz, "%.2f %s", v, units[u]);
}

static void print_logo(void)
{
    printf(C_SILVER
           "       _..._\n"
           "     .' .::::.\n"
           "    :  ::::::::\n"
           "    :  ::::::::\n"
           "    `. '::::::'\n"
           "      `-.::''\n" C_RESET);
    printf(C_GOLD "  A R T E M I S  " C_SILVER "— Goddess of the Burn\n" C_RESET);
    printf(C_DIM "  Version %s | The Archer of Images\n\n" C_RESET, APP_VERSION);
}

static bool iso_is_valid(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    char magic[ISO_MAGIC_LEN];
    bool ok = (pread(fd, magic, ISO_MAGIC_LEN, 0x8001) == ISO_MAGIC_LEN) &&
              (memcmp(magic, ISO_MAGIC, ISO_MAGIC_LEN) == 0);
    
    if (!ok) {
        ok = (pread(fd, magic, ISO_MAGIC_LEN, 0x8801) == ISO_MAGIC_LEN) &&
             (memcmp(magic, ISO_MAGIC, ISO_MAGIC_LEN) == 0);
    }

    close(fd);
    return ok;
}

static bool has_partition_table(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    struct mbr m;
    bool ok = (pread(fd, &m, sizeof(m), 0) == sizeof(m)) && (m.signature == 0xAA55);
    
    if (ok) {
        bool found = false;
        for (int i = 0; i < 4; i++) {
            if (m.parts[i].type == 0xEE || m.parts[i].lba_len > 0) { 
                found = true; 
                break; 
            }
        }
        ok = found;
    }

    close(fd);
    return ok;
}

static uint32_t find_el_torito_boot_lba(int fd)
{
    uint8_t buf[2048];
    for (int lba = 16; lba < 64; lba++) {
        if (pread(fd, buf, 2048, (uint64_t)lba * 2048) != 2048) continue;
        if (buf[0] == 0x00 && memcmp(buf + 1, "CD001", 5) == 0 && memcmp(buf + 7, "EL TORITO", 9) == 0) {
            uint32_t catalog_lba = *(uint32_t*)(buf + 0x47);
            if (pread(fd, buf, 2048, (uint64_t)catalog_lba * 2048) != 2048) return 0;
            if (buf[32] == 0x88) {
                return *(uint32_t*)(buf + 32 + 8);
            }
        }
    }
    return 0;
}

static bool device_is_mounted(const char *dev)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return false;

    const char *stem = strrchr(dev, '/');
    stem = stem ? stem + 1 : dev;

    char line[1024];
    bool mounted = false;
    while (fgets(line, sizeof(line), f)) {
        char mdev[256];
        if (sscanf(line, "%255s", mdev) != 1) continue;
        const char *mstem = strrchr(mdev, '/');
        mstem = mstem ? mstem + 1 : mdev;
        
        if (strcmp(mstem, stem) == 0) {
            mounted = true;
            break;
        }
        size_t slen = strlen(stem);
        if (strncmp(mstem, stem, slen) == 0 && (mstem[slen] == 'p' || (mstem[slen] >= '0' && mstem[slen] <= '9'))) {
            mounted = true;
            break;
        }
    }
    fclose(f);
    return mounted;
}

static void list_devices(void)
{
    printf(C_BOLD "Available removable block devices:\n" C_RESET);
    printf(C_DIM "  %-15s  %-10s  %s\n" C_RESET, "DEVICE", "SIZE", "MODEL");

    DIR *d = opendir("/sys/block");
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strncmp(ent->d_name, "loop", 4) == 0 || 
            strncmp(ent->d_name, "zram", 4) == 0 ||
            strncmp(ent->d_name, "dm-", 3) == 0) continue;
        if (strncmp(ent->d_name, "sr", 2) == 0) continue;

        char path[512];
        snprintf(path, sizeof(path), "/sys/block/%s/removable", ent->d_name);
        FILE *f = fopen(path, "r");
        int rem = 0; if (f) { fscanf(f, "%d", &rem); fclose(f); }
        
        char vendor[256] = {0};
        snprintf(path, sizeof(path), "/sys/block/%s/device/vendor", ent->d_name);
        f = fopen(path, "r");
        if (f) { fgets(vendor, sizeof(vendor), f); fclose(f); }

        if (!rem && !strstr(vendor, "USB") && !strstr(vendor, "SD")) continue;

        snprintf(path, sizeof(path), "/sys/block/%s/size", ent->d_name);
        f = fopen(path, "r");
        uint64_t sectors = 0; if (f) { fscanf(f, "%" SCNu64, &sectors); fclose(f); }
        if (sectors == 0) continue;

        char szstr[32]; fmt_size(sectors * 512, szstr, sizeof(szstr));

        snprintf(path, sizeof(path), "/sys/block/%s/device/model", ent->d_name);
        f = fopen(path, "r");
        char model[64] = "Unknown Device";
        if (f) { if (fgets(model, sizeof(model), f)) { model[strcspn(model, "\n")] = 0; } fclose(f); }

        printf("  /dev/%-10s  %-10s  %s\n", ent->d_name, szstr, model);
    }
    closedir(d);
}

static struct timespec g_start;

static void progress_init(void) { clock_gettime(CLOCK_MONOTONIC, &g_start); }

static void progress_draw(const char *label, uint64_t done, uint64_t total)
{
    double pct = total > 0 ? (double)done / total : 0;
    int filled = (int)(pct * PROGRESS_WIDTH);
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - g_start.tv_sec) + (now.tv_nsec - g_start.tv_nsec) * 1e-9;
    double speed = elapsed > 0 ? (double)done / elapsed : 0;
    double eta = (speed > 100 && total > done) ? (total - done) / speed : 0;

    char d_str[16], t_str[16], s_str[16];
    fmt_size(done, d_str, 16); fmt_size(total, t_str, 16); fmt_size((uint64_t)speed, s_str, 16);

    printf("\r  " C_SKY "%-8s " C_GOLD "[", label);
    for (int i = 0; i < PROGRESS_WIDTH; i++) {
        if (i < filled) printf("━");
        else if (i == filled) printf("⏵");
        else printf(" ");
    }
    printf("]" C_RESET " %5.1f%% %s/%s ", pct * 100, d_str, t_str);
    if (eta > 0) printf("ETA %02d:%02d    ", (int)eta/60, (int)eta%60);
    else printf("               ");
    fflush(stdout);
}

static const uint8_t generic_mbr[446] = {
    0x33, 0xc0, 0x8e, 0xd0, 0xbc, 0x00, 0x7c, 0x8e, 0xc0, 0x8e, 0xd8, 0xbe, 0x00, 0x7c, 0xbf, 0x00,
    0x06, 0xb9, 0x00, 0x02, 0xf3, 0xa4, 0xea, 0x1d, 0x06, 0x00, 0x00, 0xbe, 0xbe, 0x07, 0xb1, 0x04,
    0x80, 0x3c, 0x80, 0x74, 0x0e, 0x80, 0x3c, 0x00, 0x75, 0x1c, 0x83, 0xc6, 0x10, 0xe2, 0xec, 0xcd,
    0x18, 0x8b, 0x14, 0x8b, 0xee, 0x83, 0xc6, 0x10, 0x49, 0x74, 0x05, 0x38, 0x0c, 0x74, 0xf6, 0xbe,
    0x10, 0x07, 0x4e, 0xac, 0x3c, 0x00, 0x74, 0x0b, 0x56, 0xbb, 0x07, 0x00, 0xb4, 0x0e, 0xcd, 0x10,
    0x5e, 0xeb, 0xf0, 0xeb, 0xfe, 0xbf, 0x05, 0x00, 0xbb, 0x00, 0x7c, 0xb8, 0x01, 0x02, 0x57, 0xcd,
    0x13, 0x5f, 0x73, 0x0c, 0x33, 0xc0, 0xcd, 0x13, 0x4f, 0x75, 0xed, 0xbe, 0xa3, 0x06, 0xeb, 0xd3,
    0x81, 0x3e, 0xfe, 0x7d, 0x55, 0xaa, 0x75, 0xe7, 0x8b, 0xf5, 0xea, 0x00, 0x7c, 0x00, 0x00
};

static void burn(const char *iso_path, const char *dev_path, bool hybridize, bool verify)
{
    print_logo();
    struct stat iso_st; if (stat(iso_path, &iso_st) != 0) die(iso_path);
    int iso_fd = open(iso_path, O_RDONLY); if (iso_fd < 0) die(iso_path);

    char szstr[32]; fmt_size(iso_st.st_size, szstr, sizeof(szstr));
    printf("  " C_BOLD "Source Image: " C_RESET "%s (%s)\n", iso_path, szstr);

    bool valid = iso_is_valid(iso_path);
    bool hybrid = has_partition_table(iso_path);
    uint32_t boot_lba = find_el_torito_boot_lba(iso_fd);

    printf("  " C_BOLD "ISO Magic:    " C_RESET "%s\n", valid ? C_GREEN "Detected" C_RESET : C_YELLOW "Missing (Raw Image?)" C_RESET);
    printf("  " C_BOLD "Boot Support: " C_RESET "%s\n", hybrid ? C_GREEN "Hybrid (Ready)" C_RESET : (boot_lba ? C_SKY "Legacy (El Torito)" C_RESET : C_YELLOW "Unknown" C_RESET));

    if (device_is_mounted(dev_path)) die_msg("Target device is mounted. Please unmount first.");

    int dev_fd = open(dev_path, O_RDWR | O_EXCL);
    if (dev_fd < 0) dev_fd = open(dev_path, O_RDWR);
    if (dev_fd < 0) die(dev_path);

    uint64_t dev_bytes = 0;
#ifdef BLKGETSIZE64
    ioctl(dev_fd, BLKGETSIZE64, &dev_bytes);
#endif
    if (dev_bytes > 0) {
        char dsz[32]; fmt_size(dev_bytes, dsz, sizeof(dsz));
        printf("  " C_BOLD "Target:       " C_RESET "%s (%s)\n", dev_path, dsz);
        if ((uint64_t)iso_st.st_size > dev_bytes) die_msg("Image too large for target device.");
    }

    printf(C_RED "\n  ! WARNING: ALL DATA ON %s WILL BE DESTROYED !\n" C_RESET, dev_path);
    printf("  Confirm by typing 'yes': "); fflush(stdout);
    char confirm[16] = {0};
    if (!fgets(confirm, sizeof(confirm), stdin) || strncmp(confirm, "yes", 3) != 0) {
        printf("  Burn cancelled.\n"); close(dev_fd); close(iso_fd); return;
    }

    printf("\n  " C_SKY "Wiping existing partition table..." C_RESET " ");
    uint8_t zero[4096] = {0};
    pwrite(dev_fd, zero, 4096, 0);
    fsync(dev_fd);
#ifdef BLKFLSBUF
    ioctl(dev_fd, BLKFLSBUF, 0);
#endif
    printf(C_GREEN "done\n" C_RESET);

    void *buf;
    if (posix_memalign(&buf, 4096, BUF_SIZE) != 0) die("posix_memalign");

    uint64_t total = iso_st.st_size, written = 0;
    int sync_count = 0;
    printf(C_BOLD "  Burn!" C_RESET "\n");
    progress_init();

    while (written < total) {
        ssize_t nr = read(iso_fd, buf, BUF_SIZE);
        if (nr <= 0) break;
        
        ssize_t nw_total = 0;
        while (nw_total < nr) {
            ssize_t nw = write(dev_fd, (char*)buf + nw_total, (size_t)(nr - nw_total));
            if (nw < 0) {
                if (errno == EINTR) continue;
                die("write");
            }
            nw_total += nw;
        }
        written += nw_total;
        progress_draw("Writing", written, total);

        if (++sync_count >= 8) {
            fdatasync(dev_fd);
            sync_count = 0;
        }
    }

    printf("\n  " C_DIM "Flushing final buffers to disk..." C_RESET " ");
    fflush(stdout);
    fsync(dev_fd);
#ifdef BLKFLSBUF
    ioctl(dev_fd, BLKFLSBUF, 0);
#endif
    printf(C_GREEN "done\n" C_RESET);

    if (verify) {
        printf(C_BOLD "  Verify!" C_RESET "\n");
        lseek(iso_fd, 0, SEEK_SET);
        lseek(dev_fd, 0, SEEK_SET);
        void *vbuf;
        if (posix_memalign(&vbuf, 4096, BUF_SIZE) != 0) die("posix_memalign");
        uint64_t checked = 0;
        progress_init();
        while (checked < total) {
            ssize_t nr1 = read(iso_fd, buf, BUF_SIZE);
            if (nr1 <= 0) break;
            
            ssize_t nr2_total = 0;
            while (nr2_total < nr1) {
                ssize_t nr2 = read(dev_fd, (char*)vbuf + nr2_total, (size_t)(nr1 - nr2_total));
                if (nr2 <= 0) die_msg("Verification failed! Unexpected end of device.");
                nr2_total += nr2;
            }

            if (memcmp(buf, vbuf, (size_t)nr1) != 0) die_msg("Verification failed! Data mismatch.");
            checked += nr1;
            progress_draw("Verifying", checked, total);
        }
        free(vbuf);
        printf("\n");
    }

    if (hybridize && !hybrid) {
        bool is_xp = false;
        if (boot_lba > 0) {
            printf("  Is this a Windows XP or older image (one that needs a CD)? (y/n): "); fflush(stdout);
            char ans[16] = {0};
            if (fgets(ans, sizeof(ans), stdin) && (ans[0] == 'y' || ans[0] == 'Y')) {
                is_xp = true;
            }
        }

        printf("  " C_SKY "Synthesizing MBR for legacy/XP boot (Hybrid Fix)..." C_RESET "\n");
        struct mbr m; memset(&m, 0, sizeof(m));
        memcpy(m.code, generic_mbr, 446);
        
        m.parts[0].bootable = 0x80;
        m.parts[0].type = is_xp ? 0x07 : 0x17;
        
        m.parts[0].start_head = 0x00;
        m.parts[0].start_sec  = 0x02;
        m.parts[0].start_cyl  = 0x00;
        
        m.parts[0].end_head   = 0xFE;
        m.parts[0].end_sec    = 0x3F;
        m.parts[0].end_cyl    = 0xFF;

        m.parts[0].lba_start = 1;
        m.parts[0].lba_len = (uint32_t)(total / 512) - 1;
        
        if (boot_lba > 0) {
            printf("  " C_DIM "Legacy boot image found at sector %u\n" C_RESET, boot_lba);
        }
        
        m.signature = 0xAA55;
        pwrite(dev_fd, &m, sizeof(m), 0);
        printf(C_GREEN "  Hybridization successful (Type: 0x%02X).\n" C_RESET, m.parts[0].type);
    }

    printf("  " C_DIM "Finishing stroke (syncing)..." C_RESET " ");
    fsync(dev_fd);

#ifdef BLKFLSBUF
    ioctl(dev_fd, BLKFLSBUF, 0);
#endif
    printf(C_GREEN "done\n" C_RESET);

    close(iso_fd); close(dev_fd); free(buf);
    printf(C_BOLD "\n  Successfully finished Artemis burn to %s\n" C_RESET, dev_path);

    printf("  " C_CYAN "★ " C_GOLD "Artemis by solace-jpg " C_CYAN "★\n\n" C_RESET);
}

static void usage(const char *prog)
{
    printf("Artemis %s - ISO Burning Utility\n", APP_VERSION);
    printf("Usage: sudo %s [options] <image.iso> <device>\n\n", prog);
    printf("Options:\n"
           "  -l, --list        List removable devices\n"
           "  -n, --no-hybrid   Skip legacy hybridization\n"
           "  -v, --verify      Verify after burn\n"
           "  -h, --help        Show this help\n\n");
}

int main(int argc, char *argv[])
{
    bool list = false, hybridize = true, verify = false;
    static struct option long_opts[] = {
        {"list", no_argument, 0, 'l'},
        {"no-hybrid", no_argument, 0, 'n'},
        {"verify", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "lnvh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'l': list = true; break;
            case 'n': hybridize = false; break;
            case 'v': verify = true; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    if (list) { list_devices(); return 0; }
    if (optind + 2 > argc) { usage(argv[0]); return 1; }
    if (geteuid() != 0) {
        fprintf(stderr, C_RED "error: " C_RESET "Artemis requires root privileges.\n");
        return 1;
    }

    burn(argv[optind], argv[optind+1], hybridize, verify);
    return 0;
}
