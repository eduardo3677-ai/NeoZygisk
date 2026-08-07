#include "dex.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

#include <android/log.h>

#ifndef LOG_TAG
#if defined(__LP64__)
#define LOG_TAG "zygisk-core64"
#else
#define LOG_TAG "zygisk-core32"
#endif
#endif

// Local logging (keeps dex.cpp dependency-light, mirroring injector style).
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)

namespace dex {

// -------------------------------------------------------------------------
// Minimal RAII for a raw mmap region backed by an open fd.
// -------------------------------------------------------------------------
namespace {

struct MappedFile {
    void *addr = nullptr;
    size_t size = 0;
    bool ok = false;

    ~MappedFile() {
        if (addr != nullptr) munmap(addr, size);
    }

    MappedFile() = default;
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;

    MappedFile(MappedFile &&other) noexcept
        : addr(other.addr), size(other.size), ok(other.ok) {
        other.addr = nullptr;
        other.size = 0;
        other.ok = false;
    }

    MappedFile &operator=(MappedFile &&other) noexcept {
        if (this != &other) {
            if (addr != nullptr) munmap(addr, size);
            addr = other.addr;
            size = other.size;
            ok = other.ok;
            other.addr = nullptr;
            other.size = 0;
            other.ok = false;
        }
        return *this;
    }
};

// Maps an open fd (must have been opened via openat on the module dir) fully.
MappedFile map_whole_file(int fd) {
    MappedFile out;
    struct stat st {};
    if (fstat(fd, &st) != 0) return out;
    if (st.st_size <= 0) return out;
    void *addr = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (addr == MAP_FAILED) return out;
    out.addr = addr;
    out.size = static_cast<size_t>(st.st_size);
    out.ok = true;
    return out;
}

}  // namespace

// -------------------------------------------------------------------------
// Core injection routine.
// -------------------------------------------------------------------------

bool injectModuleDex(JNIEnv *env, int dir_fd, const char *dir_label) {
    if (env == nullptr || dir_fd < 0) return false;

    // Resolve the BootClassLoader, which will become the parent of the
    // InMemoryDexClassLoader we create for the module's dex.
    jclass class_loader_cls = env->FindClass("java/lang/ClassLoader");
    if (class_loader_cls == nullptr) {
        env->ExceptionClear();
        LOGE("[%s] failed to find java/lang/ClassLoader", dir_label);
        return false;
    }
    jmethodID get_system_cl =
        env->GetStaticMethodID(class_loader_cls, "getSystemClassLoader",
                               "()Ljava/lang/ClassLoader;");
    if (get_system_cl == nullptr) {
        env->ExceptionClear();
        LOGE("[%s] failed to find getSystemClassLoader", dir_label);
        env->DeleteLocalRef(class_loader_cls);
        return false;
    }
    jobject parent_cl = env->CallStaticObjectMethod(class_loader_cls, get_system_cl);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(class_loader_cls);
        LOGE("[%s] getSystemClassLoader failed", dir_label);
        return false;
    }

    // Locate the classes: read classes.dex, classes2.dex, ... classesN.dex.
    // The loop stops at the first missing index (standard APK-like layout).
    constexpr int kMaxDexFiles = 64;
    int fd_list[kMaxDexFiles];
    int dex_count = 0;

    for (int i = 0; i < kMaxDexFiles; ++i) {
        char name[32];
        if (i == 0) {
            snprintf(name, sizeof(name), "classes.dex");
        } else {
            snprintf(name, sizeof(name), "classes%d.dex", i + 1);
        }
        int fd = openat(dir_fd, name, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (i == 0) {
                LOGV("[%s] no %s present, nothing to inject", dir_label, name);
            }
            break;
        }
        fd_list[dex_count++] = fd;
    }

    if (dex_count == 0) {
        env->DeleteLocalRef(parent_cl);
        env->DeleteLocalRef(class_loader_cls);
        return false;
    }

    // JNI machinery for InMemoryDexClassLoader.
    jclass in_mem_cls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (in_mem_cls == nullptr) {
        env->ExceptionClear();
        LOGE("[%s] failed to find InMemoryDexClassLoader", dir_label);
        for (int i = 0; i < dex_count; ++i) close(fd_list[i]);
        env->DeleteLocalRef(parent_cl);
        env->DeleteLocalRef(class_loader_cls);
        return false;
    }
    jmethodID in_mem_ctor = env->GetMethodID(
        in_mem_cls, "<init>", "([Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    if (in_mem_ctor == nullptr) {
        env->ExceptionClear();
        LOGE("[%s] failed to find InMemoryDexClassLoader ctor", dir_label);
        for (int i = 0; i < dex_count; ++i) close(fd_list[i]);
        env->DeleteLocalRef(in_mem_cls);
        env->DeleteLocalRef(parent_cl);
        env->DeleteLocalRef(class_loader_cls);
        return false;
    }

    jclass byte_buffer_cls = env->FindClass("java/nio/ByteBuffer");
    if (byte_buffer_cls == nullptr) {
        env->ExceptionClear();
        LOGE("[%s] failed to find java/nio/ByteBuffer", dir_label);
        for (int i = 0; i < dex_count; ++i) close(fd_list[i]);
        env->DeleteLocalRef(in_mem_cls);
        env->DeleteLocalRef(parent_cl);
        env->DeleteLocalRef(class_loader_cls);
        return false;
    }
    jmethodID wrap_method = env->GetStaticMethodID(
        byte_buffer_cls, "wrap", "([B)Ljava/nio/ByteBuffer;");
    if (wrap_method == nullptr) {
        env->ExceptionClear();
        LOGE("[%s] failed to find ByteBuffer.wrap", dir_label);
        for (int i = 0; i < dex_count; ++i) close(fd_list[i]);
        env->DeleteLocalRef(byte_buffer_cls);
        env->DeleteLocalRef(in_mem_cls);
        env->DeleteLocalRef(parent_cl);
        env->DeleteLocalRef(class_loader_cls);
        return false;
    }

    // Build the ByteBuffer[] array from the mapped dex files.
    jobjectArray buffers = env->NewObjectArray(dex_count, byte_buffer_cls, nullptr);
    if (buffers == nullptr) {
        env->ExceptionClear();
        LOGE("[%s] failed to allocate ByteBuffer[]", dir_label);
        for (int i = 0; i < dex_count; ++i) close(fd_list[i]);
        env->DeleteLocalRef(byte_buffer_cls);
        env->DeleteLocalRef(in_mem_cls);
        env->DeleteLocalRef(parent_cl);
        env->DeleteLocalRef(class_loader_cls);
        return false;
    }

    int mapped = 0;
    for (int i = 0; i < dex_count; ++i) {
        MappedFile mf = map_whole_file(fd_list[i]);
        close(fd_list[i]);
        if (!mf.ok) {
            LOGW("[%s] failed to map %s", dir_label, i == 0 ? "classes.dex" : "classesN.dex");
            continue;
        }
        jbyteArray bytes = env->NewByteArray(static_cast<jsize>(mf.size));
        if (bytes == nullptr) {
            env->ExceptionClear();
            LOGW("[%s] NewByteArray failed", dir_label);
            continue;
        }
        env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(mf.size),
                                static_cast<const jbyte *>(mf.addr));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            env->DeleteLocalRef(bytes);
            LOGW("[%s] SetByteArrayRegion failed", dir_label);
            continue;
        }
        jobject buf = env->CallStaticObjectMethod(byte_buffer_cls, wrap_method, bytes);
        env->DeleteLocalRef(bytes);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGW("[%s] ByteBuffer.wrap failed", dir_label);
            continue;
        }
        env->SetObjectArrayElement(buffers, mapped, buf);
        env->DeleteLocalRef(buf);
        if (env->ExceptionCheck()) env->ExceptionClear();
        ++mapped;
    }

    if (mapped == 0) {
        LOGE("[%s] no dex could be mapped", dir_label);
        env->DeleteLocalRef(buffers);
        env->DeleteLocalRef(byte_buffer_cls);
        env->DeleteLocalRef(in_mem_cls);
        env->DeleteLocalRef(parent_cl);
        env->DeleteLocalRef(class_loader_cls);
        return false;
    }

    // If fewer dex mapped than opened, trim the array to what actually mapped.
    if (mapped < dex_count) {
        jobjectArray trimmed = env->NewObjectArray(mapped, byte_buffer_cls, nullptr);
        if (trimmed != nullptr) {
            for (int i = 0; i < mapped; ++i) {
                jobject e = env->GetObjectArrayElement(buffers, i);
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    break;
                }
                env->SetObjectArrayElement(trimmed, i, e);
                env->DeleteLocalRef(e);
            }
            env->DeleteLocalRef(buffers);
            buffers = trimmed;
        }
    }

    // Create the InMemoryDexClassLoader rooted at the boot classloader. Keep a
    // strong (global) reference so the loaded classes outlive the frame.
    jobject loader =
        env->NewObject(in_mem_cls, in_mem_ctor, buffers, parent_cl);
    if (env->ExceptionCheck() || loader == nullptr) {
        env->ExceptionClear();
        LOGE("[%s] InMemoryDexClassLoader construction failed", dir_label);
        env->DeleteLocalRef(buffers);
        env->DeleteLocalRef(byte_buffer_cls);
        env->DeleteLocalRef(in_mem_cls);
        env->DeleteLocalRef(parent_cl);
        env->DeleteLocalRef(class_loader_cls);
        return false;
    }

    jobject global_loader = env->NewGlobalRef(loader);
    env->DeleteLocalRef(loader);
    env->DeleteLocalRef(buffers);
    env->DeleteLocalRef(byte_buffer_cls);
    env->DeleteLocalRef(in_mem_cls);
    env->DeleteLocalRef(parent_cl);
    env->DeleteLocalRef(class_loader_cls);

    if (global_loader == nullptr) {
        LOGW("[%s] InMemoryDexClassLoader loaded but global ref unavailable", dir_label);
        return true;
    }

    LOGV("[%s] injected %d dex file(s) via InMemoryDexClassLoader", dir_label, mapped);
    // Intentionally leak the global ref: the classloader must live for the whole
    // lifetime of the target process so the module's Java classes stay resolvable.
    (void)global_loader;
    return true;
}

}  // namespace dex
