#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <link.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

JNIEXPORT void JNICALL
Java_com_droiddeck_launcher_input_FakeInputWriter_nativeStoreFence(JNIEnv *env, jclass clazz) {
    (void)env;
    (void)clazz;
    atomic_thread_fence(memory_order_release);
}

JNIEXPORT jint JNICALL
Java_com_droiddeck_launcher_session_ThreadSignals_tgkill(JNIEnv *env, jclass clazz, jint tgid, jint tid, jint signal) {
    (void)env;
    (void)clazz;
    return syscall(SYS_tgkill, tgid, tid, signal) == 0 ? 0 : errno;
}

struct migui_lib {
    const char *path;
    uintptr_t bias;
};

static int find_migui(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    const char *name = info->dlpi_name;
    size_t n = name ? strlen(name) : 0;
    if (n < 12 || strcmp(name + n - 12, "/libmigui.so") != 0) return 0;
    struct migui_lib *lib = data;
    lib->path = name;
    lib->bias = info->dlpi_addr;
    return 1;
}

/* The library is outside this app's linker namespace, so its exported symbol is read from the file. */
static uintptr_t elf_symbol(const char *path, const char *symbol) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    uintptr_t value = 0;
    if (fstat(fd, &st) == 0 && st.st_size > (off_t)sizeof(Elf64_Ehdr)) {
        const uint8_t *base = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (base != MAP_FAILED) {
            const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
            size_t size = st.st_size;
            if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 && eh->e_ident[EI_CLASS] == ELFCLASS64 &&
                eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf64_Shdr) <= size) {
                const Elf64_Shdr *sh = (const Elf64_Shdr *)(base + eh->e_shoff);
                for (int i = 0; i < eh->e_shnum && !value; i++) {
                    if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
                    const Elf64_Shdr *strs = &sh[sh[i].sh_link];
                    if (sh[i].sh_offset + sh[i].sh_size > size || strs->sh_offset + strs->sh_size > size) continue;
                    const Elf64_Sym *syms = (const Elf64_Sym *)(base + sh[i].sh_offset);
                    const char *names = (const char *)(base + strs->sh_offset);
                    size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
                    for (size_t k = 0; k < count; k++) {
                        if (syms[k].st_name >= strs->sh_size || syms[k].st_shndx == SHN_UNDEF) continue;
                        if (strcmp(names + syms[k].st_name, symbol) == 0) {
                            value = syms[k].st_value;
                            break;
                        }
                    }
                }
            }
            munmap((void *)base, st.st_size);
        }
    }
    close(fd);
    return value;
}

/*
 * [mali-g2] HyperOS's libmigui (loaded into every app through libgui) reports this process's BufferQueue
 * frames to SchedGenius when system_server names the package as the top game; SchedGenius then sizes the
 * GPU clock by those layers, which carry neither the game's frames nor its GPU work. FrameSchedPolicy's
 * first byte is its "initialized" flag, checked by notifyFrameSched and set only by its constructor.
 * The pid and process name stored after it (a libc++ string) are compared first so another layout is left alone.
 * Returns 1 when reports were turned off, 0 when libmigui is not loaded, negative when it does not match.
 */
JNIEXPORT jint JNICALL
Java_com_droiddeck_launcher_session_FrameSchedOptOut_nativeDisable(JNIEnv *env, jclass clazz, jstring process) {
    (void)clazz;
    struct migui_lib lib = {0};
    dl_iterate_phdr(find_migui, &lib);
    if (!lib.path) return 0;
    uintptr_t get_instance = elf_symbol(lib.path, "_ZN7android16FrameSchedPolicy11getInstanceEv");
    if (!get_instance || !elf_symbol(lib.path, "_ZN7android16FrameSchedPolicy16notifyFrameSchedEiilillilRKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEb"))
        return -1;
    uint8_t *policy = ((uint8_t *(*)(void))(lib.bias + get_instance))();
    if (!policy) return -2;
    const char *name = (*env)->GetStringUTFChars(env, process, NULL);
    if (!name) return -3;
    size_t want = strlen(name);
    const uint8_t *str = policy + 8;
    size_t len;
    const char *data;
    if (str[0] & 1) {
        len = *(const size_t *)(str + 8);
        data = *(const char *const *)(str + 16);
    } else {
        len = str[0] >> 1;
        data = (const char *)str + 1;
    }
    /* Read from /proc/<pid>/cmdline with its NUL padding. */
    int named = data && len >= want && memcmp(data, name, want) == 0 && (len == want || data[want] == 0);
    (*env)->ReleaseStringUTFChars(env, process, name);
    if (*(const int32_t *)(policy + 4) != getpid()) return -4;
    if (!named) return -6;
    if (__atomic_load_n(policy, __ATOMIC_ACQUIRE) != 1) return -5;
    __atomic_store_n(policy, 0, __ATOMIC_RELEASE);
    return 1;
}
