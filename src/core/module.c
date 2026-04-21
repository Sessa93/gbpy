/*
 * Python C Extension Module for gbpy
 *
 * Exposes the Emulator as a Python type with methods for:
 * - Loading ROMs
 * - Running frames
 * - Getting framebuffer data
 * - Getting audio samples
 * - Button input
 * - Save/load states
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "emulator.h"
#include "memory/mmu.h"

/* ---------- EmulatorObject ---------- */

typedef struct {
    PyObject_HEAD
    Emulator emu;
} EmulatorObject;

static PyObject *Emulator_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    EmulatorObject *self = (EmulatorObject *)type->tp_alloc(type, 0);
    if (self) {
        emu_init(&self->emu);
    }
    return (PyObject *)self;
}

static void Emulator_dealloc(EmulatorObject *self) {
    emu_destroy(&self->emu);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

/* load_rom(data: bytes) -> None */
static PyObject *Emulator_load_rom(EmulatorObject *self, PyObject *args) {
    Py_buffer buf;
    if (!PyArg_ParseTuple(args, "y*", &buf))
        return NULL;

    int result = emu_load_rom(&self->emu, (const uint8_t *)buf.buf, buf.len);
    PyBuffer_Release(&buf);

    if (result != GBPY_OK) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to load ROM");
        return NULL;
    }
    Py_RETURN_NONE;
}

/* load_rom_file(path: str) -> None */
static PyObject *Emulator_load_rom_file(EmulatorObject *self, PyObject *args) {
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path))
        return NULL;

    int result = emu_load_rom_file(&self->emu, path);
    if (result != GBPY_OK) {
        PyErr_Format(PyExc_RuntimeError, "Failed to load ROM file: %s", path);
        return NULL;
    }
    Py_RETURN_NONE;
}

/* reset() -> None */
static PyObject *Emulator_reset(EmulatorObject *self, PyObject *Py_UNUSED(args)) {
    emu_reset(&self->emu);
    Py_RETURN_NONE;
}

/* run_frame() -> None */
static PyObject *Emulator_run_frame(EmulatorObject *self, PyObject *Py_UNUSED(args)) {
    Py_BEGIN_ALLOW_THREADS
    emu_run_frame(&self->emu);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

/* step(n: int = 1) -> None */
static PyObject *Emulator_step(EmulatorObject *self, PyObject *args) {
    uint32_t steps = 1;
    if (!PyArg_ParseTuple(args, "|I", &steps))
        return NULL;
    Py_BEGIN_ALLOW_THREADS
    emu_step(&self->emu, steps);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

/* button_press(button: int) -> None */
static PyObject *Emulator_button_press(EmulatorObject *self, PyObject *args) {
    int btn;
    if (!PyArg_ParseTuple(args, "i", &btn))
        return NULL;
    if (btn < 0 || btn > BTN_R) {
        PyErr_SetString(PyExc_ValueError, "Invalid button");
        return NULL;
    }
    emu_button_press(&self->emu, (Button)btn);
    Py_RETURN_NONE;
}

/* button_release(button: int) -> None */
static PyObject *Emulator_button_release(EmulatorObject *self, PyObject *args) {
    int btn;
    if (!PyArg_ParseTuple(args, "i", &btn))
        return NULL;
    if (btn < 0 || btn > BTN_R) {
        PyErr_SetString(PyExc_ValueError, "Invalid button");
        return NULL;
    }
    emu_button_release(&self->emu, (Button)btn);
    Py_RETURN_NONE;
}

/* get_framebuffer() -> bytes
 * Returns RGBA pixel data (width * height * 4 bytes)
 */
static PyObject *Emulator_get_framebuffer(EmulatorObject *self, PyObject *Py_UNUSED(args)) {
    const uint8_t *fb = emu_get_framebuffer(&self->emu);
    if (!fb) {
        Py_RETURN_NONE;
    }
    int w = emu_get_screen_width(&self->emu);
    int h = emu_get_screen_height(&self->emu);
    return PyBytes_FromStringAndSize((const char *)fb, w * h * 4);
}

/* get_framebuffer_array() -> memoryview
 * Returns a read-only buffer over the internal framebuffer (zero-copy)
 */
static int Emulator_getbuffer(EmulatorObject *self, Py_buffer *view, int flags) {
    const uint8_t *fb = emu_get_framebuffer(&self->emu);
    if (!fb) {
        PyErr_SetString(PyExc_BufferError, "No ROM loaded");
        return -1;
    }
    int w = emu_get_screen_width(&self->emu);
    int h = emu_get_screen_height(&self->emu);
    return PyBuffer_FillInfo(view, (PyObject *)self, (void *)fb, w * h * 4, 1, flags);
}

/* screen_width -> int */
static PyObject *Emulator_get_screen_width(EmulatorObject *self, void *closure) {
    (void)closure;
    return PyLong_FromLong(emu_get_screen_width(&self->emu));
}

/* screen_height -> int */
static PyObject *Emulator_get_screen_height(EmulatorObject *self, void *closure) {
    (void)closure;
    return PyLong_FromLong(emu_get_screen_height(&self->emu));
}

/* mode -> str ("GB", "GBC", or "GBA") */
static PyObject *Emulator_get_mode(EmulatorObject *self, void *closure) {
    (void)closure;
    switch (self->emu.mode) {
        case GBPY_MODE_GB:  return PyUnicode_FromString("GB");
        case GBPY_MODE_GBC: return PyUnicode_FromString("GBC");
        case GBPY_MODE_GBA: return PyUnicode_FromString("GBA");
        default:            return PyUnicode_FromString("Unknown");
    }
}

/* running -> bool */
static PyObject *Emulator_get_running(EmulatorObject *self, void *closure) {
    (void)closure;
    return PyBool_FromLong(self->emu.running);
}

/* get_audio() -> bytes (interleaved int16 L/R samples) */
static PyObject *Emulator_get_audio(EmulatorObject *self, PyObject *Py_UNUSED(args)) {
    uint32_t count = emu_get_audio_samples(&self->emu);
    if (count == 0) {
        return PyBytes_FromStringAndSize(NULL, 0);
    }
    const int16_t *buf = emu_get_audio_buffer(&self->emu);
    PyObject *result = PyBytes_FromStringAndSize((const char *)buf,
                                                  count * 2 * sizeof(int16_t));
    emu_consume_audio(&self->emu);
    return result;
}

/* save_state(path: str) -> None */
static PyObject *Emulator_save_state(EmulatorObject *self, PyObject *args) {
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path))
        return NULL;
    int result = emu_save_state(&self->emu, path);
    if (result != GBPY_OK) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to save state");
        return NULL;
    }
    Py_RETURN_NONE;
}

/* load_state(path: str) -> None */
static PyObject *Emulator_load_state(EmulatorObject *self, PyObject *args) {
    const char *path;
    if (!PyArg_ParseTuple(args, "s", &path))
        return NULL;
    int result = emu_load_state(&self->emu, path);
    if (result != GBPY_OK) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to load state");
        return NULL;
    }
    Py_RETURN_NONE;
}

/* save_ram() -> None */
static PyObject *Emulator_save_ram(EmulatorObject *self, PyObject *Py_UNUSED(args)) {
    int result = emu_save_ram(&self->emu);
    if (result != GBPY_OK) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to save RAM");
        return NULL;
    }
    Py_RETURN_NONE;
}

/* load_ram() -> None */
static PyObject *Emulator_load_ram(EmulatorObject *self, PyObject *Py_UNUSED(args)) {
    int result = emu_load_ram(&self->emu);
    if (result != GBPY_OK) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to load RAM");
        return NULL;
    }
    Py_RETURN_NONE;
}

/* frame_ready -> bool */
static PyObject *Emulator_get_frame_ready(EmulatorObject *self, void *closure) {
    (void)closure;
    return PyBool_FromLong(self->emu.frame_ready);
}

/* get_serial_output() -> str  (GB/GBC serial port capture) */
static PyObject *Emulator_get_serial_output(EmulatorObject *self, PyObject *Py_UNUSED(args)) {
    if (self->emu.mode == GBPY_MODE_GB || self->emu.mode == GBPY_MODE_GBC) {
        return PyUnicode_FromStringAndSize(self->emu.mmu.serial_buf,
                                           self->emu.mmu.serial_pos);
    }
    return PyUnicode_FromString("");
}

/* get_cpu_registers() -> dict  (GB/GBC SM83 registers) */
static PyObject *Emulator_get_cpu_registers(EmulatorObject *self, PyObject *Py_UNUSED(args)) {
    PyObject *d = PyDict_New();
    if (!d) return NULL;
    if (self->emu.mode == GBPY_MODE_GB || self->emu.mode == GBPY_MODE_GBC) {
        SM83 *cpu = &self->emu.cpu_sm83;
        PyDict_SetItemString(d, "a",  PyLong_FromLong(cpu->a));
        PyDict_SetItemString(d, "f",  PyLong_FromLong(cpu->f));
        PyDict_SetItemString(d, "b",  PyLong_FromLong(cpu->b));
        PyDict_SetItemString(d, "c",  PyLong_FromLong(cpu->c));
        PyDict_SetItemString(d, "d",  PyLong_FromLong(cpu->d));
        PyDict_SetItemString(d, "e",  PyLong_FromLong(cpu->e));
        PyDict_SetItemString(d, "h",  PyLong_FromLong(cpu->h));
        PyDict_SetItemString(d, "l",  PyLong_FromLong(cpu->l));
        PyDict_SetItemString(d, "sp", PyLong_FromLong(cpu->sp));
        PyDict_SetItemString(d, "pc", PyLong_FromLong(cpu->pc));
        PyDict_SetItemString(d, "halted", PyBool_FromLong(cpu->halted));
    }
    return d;
}

/* read_memory(addr: int) -> int  (read a byte from GB/GBC address space) */
static PyObject *Emulator_read_memory(EmulatorObject *self, PyObject *args) {
    unsigned int addr;
    if (!PyArg_ParseTuple(args, "I", &addr))
        return NULL;
    if (self->emu.mode == GBPY_MODE_GB || self->emu.mode == GBPY_MODE_GBC) {
        if (addr > 0xFFFF) {
            PyErr_SetString(PyExc_ValueError, "Address out of range");
            return NULL;
        }
        uint8_t val = mmu_read(&self->emu.mmu, (uint16_t)addr);
        return PyLong_FromLong(val);
    }
    PyErr_SetString(PyExc_RuntimeError, "read_memory only for GB/GBC");
    return NULL;
}

/* ---------- Method table ---------- */

static PyMethodDef Emulator_methods[] = {
    {"load_rom",      (PyCFunction)Emulator_load_rom,      METH_VARARGS, "Load ROM from bytes"},
    {"load_rom_file", (PyCFunction)Emulator_load_rom_file, METH_VARARGS, "Load ROM from file path"},
    {"reset",         (PyCFunction)Emulator_reset,         METH_NOARGS,  "Reset emulator"},
    {"run_frame",     (PyCFunction)Emulator_run_frame,     METH_NOARGS,  "Run one frame"},
    {"step",          (PyCFunction)Emulator_step,          METH_VARARGS, "Step N CPU instructions"},
    {"button_press",  (PyCFunction)Emulator_button_press,  METH_VARARGS, "Press a button"},
    {"button_release",(PyCFunction)Emulator_button_release,METH_VARARGS, "Release a button"},
    {"get_framebuffer",(PyCFunction)Emulator_get_framebuffer, METH_NOARGS, "Get RGBA framebuffer bytes"},
    {"get_audio",     (PyCFunction)Emulator_get_audio,     METH_NOARGS,  "Get audio samples"},
    {"save_state",    (PyCFunction)Emulator_save_state,    METH_VARARGS, "Save state to file"},
    {"load_state",    (PyCFunction)Emulator_load_state,    METH_VARARGS, "Load state from file"},
    {"save_ram",      (PyCFunction)Emulator_save_ram,      METH_NOARGS,  "Save battery RAM"},
    {"load_ram",      (PyCFunction)Emulator_load_ram,      METH_NOARGS,  "Load battery RAM"},
    {"get_serial_output", (PyCFunction)Emulator_get_serial_output, METH_NOARGS, "Get serial output string"},
    {"get_cpu_registers", (PyCFunction)Emulator_get_cpu_registers, METH_NOARGS, "Get CPU register dict"},
    {"read_memory",   (PyCFunction)Emulator_read_memory,   METH_VARARGS, "Read a byte from memory"},
    {NULL}
};

/* ---------- Getset table ---------- */

static PyGetSetDef Emulator_getset[] = {
    {"screen_width",  (getter)Emulator_get_screen_width,  NULL, "Screen width in pixels", NULL},
    {"screen_height", (getter)Emulator_get_screen_height, NULL, "Screen height in pixels", NULL},
    {"mode",          (getter)Emulator_get_mode,          NULL, "Emulation mode (GB/GBC/GBA)", NULL},
    {"running",       (getter)Emulator_get_running,       NULL, "True if emulator is running", NULL},
    {"frame_ready",   (getter)Emulator_get_frame_ready,   NULL, "True if a frame was just completed", NULL},
    {NULL}
};

/* ---------- Buffer protocol ---------- */

static PyBufferProcs Emulator_as_buffer = {
    .bf_getbuffer = (getbufferproc)Emulator_getbuffer,
    .bf_releasebuffer = NULL,
};

/* ---------- Type definition ---------- */

static PyTypeObject EmulatorType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "gbpy._core.Emulator",
    .tp_doc       = "GBPy Emulator - supports GB, GBC, and GBA ROMs",
    .tp_basicsize = sizeof(EmulatorObject),
    .tp_itemsize  = 0,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_new       = Emulator_new,
    .tp_dealloc   = (destructor)Emulator_dealloc,
    .tp_methods   = Emulator_methods,
    .tp_getset    = Emulator_getset,
    .tp_as_buffer = &Emulator_as_buffer,
};

/* ---------- Module-level constants ---------- */

static int add_button_constants(PyObject *m) {
    if (PyModule_AddIntConstant(m, "BTN_A",      BTN_A)      < 0) return -1;
    if (PyModule_AddIntConstant(m, "BTN_B",      BTN_B)      < 0) return -1;
    if (PyModule_AddIntConstant(m, "BTN_SELECT", BTN_SELECT) < 0) return -1;
    if (PyModule_AddIntConstant(m, "BTN_START",  BTN_START)  < 0) return -1;
    if (PyModule_AddIntConstant(m, "BTN_RIGHT",  BTN_RIGHT)  < 0) return -1;
    if (PyModule_AddIntConstant(m, "BTN_LEFT",   BTN_LEFT)   < 0) return -1;
    if (PyModule_AddIntConstant(m, "BTN_UP",     BTN_UP)     < 0) return -1;
    if (PyModule_AddIntConstant(m, "BTN_DOWN",   BTN_DOWN)   < 0) return -1;
    if (PyModule_AddIntConstant(m, "BTN_L",      BTN_L)      < 0) return -1;
    if (PyModule_AddIntConstant(m, "BTN_R",      BTN_R)      < 0) return -1;
    return 0;
}

/* ---------- Module definition ---------- */

static PyModuleDef coremodule = {
    PyModuleDef_HEAD_INIT,
    .m_name    = "gbpy._core",
    .m_doc     = "GBPy emulator core (C extension)",
    .m_size    = -1,
};

PyMODINIT_FUNC PyInit__core(void) {
    PyObject *m;

    if (PyType_Ready(&EmulatorType) < 0)
        return NULL;

    m = PyModule_Create(&coremodule);
    if (!m) return NULL;

    Py_INCREF(&EmulatorType);
    if (PyModule_AddObject(m, "Emulator", (PyObject *)&EmulatorType) < 0) {
        Py_DECREF(&EmulatorType);
        Py_DECREF(m);
        return NULL;
    }

    if (add_button_constants(m) < 0) {
        Py_DECREF(m);
        return NULL;
    }

    /* Add audio constants */
    PyModule_AddIntConstant(m, "AUDIO_SAMPLE_RATE", GBPY_AUDIO_SAMPLE_RATE);
    PyModule_AddIntConstant(m, "AUDIO_BUFFER_SIZE", GBPY_AUDIO_BUFFER_SIZE);

    return m;
}
