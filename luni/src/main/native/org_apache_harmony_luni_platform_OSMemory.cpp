/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "OSMemory"

#include "JNIHelp.h"
#include "JniConstants.h"
#include "UniquePtr.h"
#include "java_lang_Float.h"
#include "java_lang_Double.h"

#include <byteswap.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/*
 * Cached dalvik.system.VMRuntime pieces.
 */
static struct {
    jmethodID method_trackExternalAllocation;
    jmethodID method_trackExternalFree;

    jobject runtimeInstance;
} gIDCache;

template <typename T> static T cast(jint address) {
    return reinterpret_cast<T>(static_cast<uintptr_t>(address));
}

static jint OSMemory_malloc(JNIEnv* env, jclass, jint size) {
    jboolean allowed = env->CallBooleanMethod(gIDCache.runtimeInstance,
            gIDCache.method_trackExternalAllocation, static_cast<jlong>(size));
    if (!allowed) {
        LOGW("External allocation of %d bytes was rejected\n", size);
        jniThrowException(env, "java/lang/OutOfMemoryError", NULL);
        return 0;
    }

    LOGV("OSMemory alloc %d\n", size);
    void* block = malloc(size + sizeof(jlong));
    if (block == NULL) {
        jniThrowException(env, "java/lang/OutOfMemoryError", NULL);
        return 0;
    }

    /*
     * Tuck a copy of the size at the head of the buffer.  We need this
     * so OSMemory_free() knows how much memory is being freed.
     */
    jlong* result = reinterpret_cast<jlong*>(block);
    *result++ = size;
    return static_cast<jint>(reinterpret_cast<uintptr_t>(result));
}

static void OSMemory_free(JNIEnv* env, jclass, jint address) {
    jlong* p = reinterpret_cast<jlong*>(static_cast<uintptr_t>(address));
    jlong size = *--p;
    LOGV("OSMemory free %ld\n", size);
    env->CallVoidMethod(gIDCache.runtimeInstance, gIDCache.method_trackExternalFree, size);
    free(reinterpret_cast<void*>(p));
}

static void OSMemory_memset(JNIEnv*, jclass, jint dstAddress, jbyte value, jlong length) {
    memset(cast<void*>(dstAddress), value, length);
}

static void OSMemory_memmove(JNIEnv*, jclass, jint dstAddress, jint srcAddress, jlong length) {
    memmove(cast<void*>(dstAddress), cast<const void*>(srcAddress), length);
}

static jbyte OSMemory_peekByte(JNIEnv*, jclass, jint srcAddress) {
    return *cast<const jbyte*>(srcAddress);
}

static void OSMemory_peekByteArray(JNIEnv* env, jclass, jint srcAddress,
        jbyteArray dst, jint offset, jint length) {
    env->SetByteArrayRegion(dst, offset, length, cast<const jbyte*>(srcAddress));
}

static void OSMemory_pokeByte(JNIEnv*, jclass, jint dstAddress, jbyte value) {
    *cast<jbyte*>(dstAddress) = value;
}

static void OSMemory_setByteArray(JNIEnv* env, jclass,
        jint dstAddress, jbyteArray src, jint offset, jint length) {
    env->GetByteArrayRegion(src, offset, length, cast<jbyte*>(dstAddress));
}

static void swapShorts(jshort* shorts, int count) {
    jbyte* src = reinterpret_cast<jbyte*>(shorts);
    jbyte* dst = src;
    for (int i = 0; i < count; ++i) {
        jbyte b0 = *src++;
        jbyte b1 = *src++;
        *dst++ = b1;
        *dst++ = b0;
    }
}

static void swapInts(jint* ints, int count) {
    jbyte* src = reinterpret_cast<jbyte*>(ints);
    jbyte* dst = src;
    for (int i = 0; i < count; ++i) {
        jbyte b0 = *src++;
        jbyte b1 = *src++;
        jbyte b2 = *src++;
        jbyte b3 = *src++;
        *dst++ = b3;
        *dst++ = b2;
        *dst++ = b1;
        *dst++ = b0;
    }
}

static void OSMemory_setFloatArray(JNIEnv* env, jclass, jint dstAddress,
        jfloatArray src, jint offset, jint length, jboolean swap) {
    jfloat* dst = cast<jfloat*>(dstAddress);
    env->GetFloatArrayRegion(src, offset, length, dst);
    if (swap) {
        swapInts(reinterpret_cast<jint*>(dst), length);
    }
}

static void OSMemory_setIntArray(JNIEnv* env, jclass,
       jint dstAddress, jintArray src, jint offset, jint length, jboolean swap) {
    jint* dst = cast<jint*>(dstAddress);
    env->GetIntArrayRegion(src, offset, length, dst);
    if (swap) {
        swapInts(dst, length);
    }
}

static void OSMemory_setShortArray(JNIEnv* env, jclass,
       jint dstAddress, jshortArray src, jint offset, jint length, jboolean swap) {
    jshort* dst = cast<jshort*>(dstAddress);
    env->GetShortArrayRegion(src, offset, length, dst);
    if (swap) {
        swapShorts(dst, length);
    }
}

static jshort OSMemory_peekShort(JNIEnv*, jclass, jint srcAddress, jboolean swap) {
    jshort result;
    if ((srcAddress & 0x1) == 0) {
        result = *cast<const jshort*>(srcAddress);
    } else {
        // Handle unaligned memory access one byte at a time
        const jbyte* src = cast<const jbyte*>(srcAddress);
        jbyte* dst = reinterpret_cast<jbyte*>(&result);
        dst[0] = src[0];
        dst[1] = src[1];
    }
    if (swap) {
        result = bswap_16(result);
    }
    return result;
}

static void OSMemory_pokeShort(JNIEnv*, jclass, jint dstAddress, jshort value, jboolean swap) {
    if (swap) {
        value = bswap_16(value);
    }
    if ((dstAddress & 0x1) == 0) {
        *cast<jshort*>(dstAddress) = value;
    } else {
        // Handle unaligned memory access one byte at a time
        const jbyte* src = reinterpret_cast<const jbyte*>(&value);
        jbyte* dst = cast<jbyte*>(dstAddress);
        dst[0] = src[0];
        dst[1] = src[1];
    }
}

static jint OSMemory_peekInt(JNIEnv*, jclass, jint srcAddress, jboolean swap) {
    jint result;
    if ((srcAddress & 0x3) == 0) {
        result = *cast<const jint*>(srcAddress);
    } else {
        // Handle unaligned memory access one byte at a time
        const jbyte* src = cast<const jbyte*>(srcAddress);
        jbyte* dst = reinterpret_cast<jbyte*>(&result);
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
    }
    if (swap) {
        result = bswap_32(result);
    }
    return result;
}

static void OSMemory_pokeInt(JNIEnv*, jclass, jint dstAddress, jint value, jboolean swap) {
    if (swap) {
        value = bswap_32(value);
    }
    if ((dstAddress & 0x3) == 0) {
        *cast<jint*>(dstAddress) = value;
    } else {
        // Handle unaligned memory access one byte at a time
        const jbyte* src = reinterpret_cast<const jbyte*>(&value);
        jbyte* dst = cast<jbyte*>(dstAddress);
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        dst[3] = src[3];
    }
}

template <typename T> static T get(jint srcAddress) {
    if ((srcAddress & (sizeof(T) - 1)) == 0) {
        return *cast<const T*>(srcAddress);
    } else {
        // Cast to void* so GCC can't assume correct alignment and optimize this out.
        const void* src = cast<const void*>(srcAddress);
        T result;
        memcpy(&result, src, sizeof(T));
        return result;
    }
}

template <typename T> static void set(jint dstAddress, T value) {
    if ((dstAddress & (sizeof(T) - 1)) == 0) {
        *cast<T*>(dstAddress) = value;
    } else {
        // Cast to void* so GCC can't assume correct alignment and optimize this out.
        void* dst = cast<void*>(dstAddress);
        memcpy(dst, &value, sizeof(T));
    }
}

static jlong OSMemory_peekLong(JNIEnv*, jclass, jint srcAddress, jboolean swap) {
    jlong result = get<jlong>(srcAddress);
    if (swap) {
        result = bswap_64(result);
    }
    return result;
}

static void OSMemory_pokeLong(JNIEnv*, jclass, jint dstAddress, jlong value, jboolean swap) {
    if (swap) {
        value = bswap_64(value);
    }
    set<jlong>(dstAddress, value);
}

static jfloat OSMemory_peekFloat(JNIEnv*, jclass, jint srcAddress, jboolean swap) {
    jfloat result = get<jfloat>(srcAddress);
    if (swap) {
        result = Float::intBitsToFloat(bswap_32(Float::floatToRawIntBits(result)));
    }
    return result;
}

static void OSMemory_pokeFloat(JNIEnv*, jclass, jint dstAddress, jfloat value, jboolean swap) {
    jint bits = Float::floatToRawIntBits(value);
    if (swap) {
        bits = bswap_32(bits);
    }
    set<jint>(dstAddress, bits);
}

static jdouble OSMemory_peekDouble(JNIEnv*, jclass, jint srcAddress, jboolean swap) {
    jdouble result = get<jdouble>(srcAddress);
    if (swap) {
        result = Double::longBitsToDouble(bswap_64(Double::doubleToRawLongBits(result)));
    }
    return result;
}

static void OSMemory_pokeDouble(JNIEnv*, jclass, jint dstAddress, jdouble value, jboolean swap) {
    jlong bits = Double::doubleToRawLongBits(value);
    if (swap) {
        bits = bswap_64(bits);
    }
    set<jlong>(dstAddress, bits);
}

static jint OSMemory_mmapImpl(JNIEnv* env, jclass, jint fd, jlong offset, jlong size, jint mapMode) {
    int prot, flags;
    switch (mapMode) {
    case 0: // MapMode.PRIVATE
        prot = PROT_READ|PROT_WRITE;
        flags = MAP_PRIVATE;
        break;
    case 1: // MapMode.READ_ONLY
        prot = PROT_READ;
        flags = MAP_SHARED;
        break;
    case 2: // MapMode.READ_WRITE
        prot = PROT_READ|PROT_WRITE;
        flags = MAP_SHARED;
        break;
    default:
        jniThrowIOException(env, EINVAL);
        LOGE("bad mapMode %i", mapMode);
        return -1;
    }

    void* mapAddress = mmap(0, size, prot, flags, fd, offset);
    if (mapAddress == MAP_FAILED) {
        jniThrowIOException(env, errno);
    }
    return reinterpret_cast<uintptr_t>(mapAddress);
}

static void OSMemory_munmap(JNIEnv*, jclass, jint address, jlong size) {
    munmap(cast<void*>(address), size);
}

static void OSMemory_load(JNIEnv*, jclass, jint address, jlong size) {
    if (mlock(cast<void*>(address), size) != -1) {
        munlock(cast<void*>(address), size);
    }
}

static jboolean OSMemory_isLoaded(JNIEnv*, jclass, jint address, jlong size) {
    if (size == 0) {
        return JNI_TRUE;
    }

    static int page_size = getpagesize();

    int align_offset = address % page_size;// addr should align with the boundary of a page.
    address -= align_offset;
    size += align_offset;
    int page_count = (size + page_size - 1) / page_size;

    UniquePtr<unsigned char[]> vec(new unsigned char[page_count]);
    int rc = mincore(cast<void*>(address), size, vec.get());
    if (rc == -1) {
        return JNI_FALSE;
    }

    for (int i = 0; i < page_count; ++i) {
        if (vec[i] != 1) {
            return JNI_FALSE;
        }
    }
    return JNI_TRUE;
}

static void OSMemory_msync(JNIEnv*, jclass, jint address, jlong size) {
    msync(cast<void*>(address), size, MS_SYNC);
}

static JNINativeMethod gMethods[] = {
    NATIVE_METHOD(OSMemory, free, "(I)V"),
    NATIVE_METHOD(OSMemory, isLoaded, "(IJ)Z"),
    NATIVE_METHOD(OSMemory, load, "(IJ)V"),
    NATIVE_METHOD(OSMemory, malloc, "(I)I"),
    NATIVE_METHOD(OSMemory, memmove, "(IIJ)V"),
    NATIVE_METHOD(OSMemory, memset, "(IBJ)V"),
    NATIVE_METHOD(OSMemory, mmapImpl, "(IJJI)I"),
    NATIVE_METHOD(OSMemory, msync, "(IJ)V"),
    NATIVE_METHOD(OSMemory, munmap, "(IJ)V"),
    NATIVE_METHOD(OSMemory, peekByte, "(I)B"),
    NATIVE_METHOD(OSMemory, peekByteArray, "(I[BII)V"),
    NATIVE_METHOD(OSMemory, peekDouble, "(IZ)D"),
    NATIVE_METHOD(OSMemory, peekFloat, "(IZ)F"),
    NATIVE_METHOD(OSMemory, peekInt, "(IZ)I"),
    NATIVE_METHOD(OSMemory, peekLong, "(IZ)J"),
    NATIVE_METHOD(OSMemory, peekShort, "(IZ)S"),
    NATIVE_METHOD(OSMemory, pokeByte, "(IB)V"),
    NATIVE_METHOD(OSMemory, pokeDouble, "(IDZ)V"),
    NATIVE_METHOD(OSMemory, pokeFloat, "(IFZ)V"),
    NATIVE_METHOD(OSMemory, pokeInt, "(IIZ)V"),
    NATIVE_METHOD(OSMemory, pokeLong, "(IJZ)V"),
    NATIVE_METHOD(OSMemory, pokeShort, "(ISZ)V"),
    NATIVE_METHOD(OSMemory, setByteArray, "(I[BII)V"),
    NATIVE_METHOD(OSMemory, setFloatArray, "(I[FIIZ)V"),
    NATIVE_METHOD(OSMemory, setIntArray, "(I[IIIZ)V"),
    NATIVE_METHOD(OSMemory, setShortArray, "(I[SIIZ)V"),
};
int register_org_apache_harmony_luni_platform_OSMemory(JNIEnv* env) {
    /*
     * We need to call VMRuntime.trackExternal{Allocation,Free}.  Cache
     * method IDs and a reference to the singleton.
     */
    gIDCache.method_trackExternalAllocation = env->GetMethodID(JniConstants::vmRuntimeClass,
        "trackExternalAllocation", "(J)Z");
    gIDCache.method_trackExternalFree = env->GetMethodID(JniConstants::vmRuntimeClass,
        "trackExternalFree", "(J)V");

    jmethodID method_getRuntime = env->GetStaticMethodID(JniConstants::vmRuntimeClass,
        "getRuntime", "()Ldalvik/system/VMRuntime;");

    if (gIDCache.method_trackExternalAllocation == NULL ||
        gIDCache.method_trackExternalFree == NULL ||
        method_getRuntime == NULL)
    {
        LOGE("Unable to find VMRuntime methods\n");
        return -1;
    }

    jobject instance = env->CallStaticObjectMethod(JniConstants::vmRuntimeClass, method_getRuntime);
    if (instance == NULL) {
        LOGE("Unable to obtain VMRuntime instance\n");
        return -1;
    }
    gIDCache.runtimeInstance = env->NewGlobalRef(instance);

    return jniRegisterNativeMethods(env, "org/apache/harmony/luni/platform/OSMemory",
            gMethods, NELEM(gMethods));
}
