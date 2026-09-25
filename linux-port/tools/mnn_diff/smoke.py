"""Smoke-test the vendor's MNN path on this host via ctypes.

Loads the prepared libmnnmodel.so (which pulls libMNN.so, libMNN_Express.so and
the APK's libc++_shared.so), drives init -> run_1 -> run_2 -> run_3, and prints
what comes back.  This is the fastest way to find out whether the bionic shims
suffice before committing to a compiled harness.
"""
import ctypes, os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SO = os.path.join(HERE, "build", "libmnnmodel.so")

lib = ctypes.CDLL(SO)
print("dlopen: OK ->", SO)

def sym(mangled):
    return getattr(lib, mangled)

init  = sym("_Z21init_mnn_model_moduleP7_JNIEnvP8_jobject")
run1  = sym("_Z9mnn_run_113mnn_mode_namei")
run2  = sym("_Z9mnn_run_213mnn_mode_namePtS0_")
run3  = sym("_Z9mnn_run_313mnn_mode_name")

init.restype = ctypes.c_int
init.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
run1.restype = ctypes.c_int
run1.argtypes = [ctypes.c_int, ctypes.c_int]
run2.restype = ctypes.c_int
run2.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint16), ctypes.POINTER(ctypes.c_uint16)]
run3.restype = ctypes.c_int
run3.argtypes = [ctypes.c_int]

MODE = 1  # MNN_ZOOM_X2

print("init(NULL, NULL) ->", init(None, None))
print("run_1(%d, 2)     ->" % MODE, run1(MODE, 2))

IN_N, OUT_N = 256 * 192, 512 * 384
inbuf = (ctypes.c_uint16 * IN_N)()
outbuf = (ctypes.c_uint16 * OUT_N)()

# A deterministic ramp in the 8-bit range the app would supply.
for i in range(IN_N):
    inbuf[i] = (i * 7) & 0xFF

rc = run2(MODE, inbuf, outbuf)
print("run_2            ->", rc)

a = np.frombuffer(outbuf, dtype=np.uint16)
print("out min %d max %d mean %.2f" % (a.min(), a.max(), a.mean()))
print("out[0:8]  :", a[:8].tolist())
print("out unique count:", len(np.unique(a)))

print("run_3(%d)        ->" % MODE, run3(MODE))
