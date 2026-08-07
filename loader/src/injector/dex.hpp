#pragma once

#include <jni.h>

/**
 * @file dex.hpp
 * @brief In-memory DEX injection for Zygisk modules.
 *
 * Some (legacy / "old API") Zygisk modules ship Java/Kotlin code as `classes*.dex`
 * files inside the module directory but rely on the host (Zygisk) to load that code
 * into the target process, instead of loading it themselves.
 *
 * This is the host-side counterpart of that feature: it reads the module's
 * `classes.dex`..`classesN.dex` files from the module directory and loads them
 * in-memory through `dalvik/system/InMemoryDexClassLoader`, so the module's Java
 * code becomes available in the forked process.
 *
 * The loading is performed exactly like the modules themselves do (byte-for-byte
 * in-memory, no dex written to disk), which keeps the implementation light and
 * avoids leaving any trace.
 *
 * The routine is strictly additive and non-fatal: if anything goes wrong it only
 * logs and aborts that module's injection; it never takes down the Zygisk/zygote
 * process or affects modules that do not rely on it.
 */

namespace dex {

/**
 * @brief Loads `classes*.dex` from a module directory into the current process.
 *
 * @param env        A valid JNIEnv in the target (forked) process.
 * @param dir_fd     An open directory file descriptor for the module directory.
 * @param dir_label  A short human-readable name for the module (used in logs).
 * @return `true` if at least one dex file was successfully injected.
 */
bool injectModuleDex(JNIEnv *env, int dir_fd, const char *dir_label);

}  // namespace dex
