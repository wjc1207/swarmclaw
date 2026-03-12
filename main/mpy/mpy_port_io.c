/* mpy_port_io.c — MicroPython port I/O for ESP32-S3 embed */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "py/runtime.h"
#include "py/stream.h"
#include "py/builtin.h"
#include "py/objstr.h"

#define SPIFFS_BASE      "/spiffs/"
#define SPIFFS_BASE_LEN  (sizeof(SPIFFS_BASE) - 1)

mp_import_stat_t mp_import_stat(const char *path)
{
    if (strncmp(path, SPIFFS_BASE, SPIFFS_BASE_LEN) != 0) {
        return MP_IMPORT_STAT_NO_EXIST;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return MP_IMPORT_STAT_NO_EXIST;
    }

    return S_ISDIR(st.st_mode) ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
}

typedef struct {
    mp_obj_base_t base;
    FILE *fp;
} mpy_file_obj_t;

static void mpy_file_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    mpy_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<file %p>", self->fp);
}

static mp_uint_t mpy_file_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode)
{
    mpy_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->fp) {
        *errcode = EBADF;
        return MP_STREAM_ERROR;
    }

    size_t n = fread(buf, 1, size, self->fp);
    if (n == 0 && ferror(self->fp)) {
        *errcode = errno;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)n;
}

static mp_uint_t mpy_file_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode)
{
    mpy_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->fp) {
        *errcode = EBADF;
        return MP_STREAM_ERROR;
    }

    size_t n = fwrite(buf, 1, size, self->fp);
    if (n < size) {
        *errcode = errno;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)n;
}

static mp_uint_t mpy_file_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode)
{
    mpy_file_obj_t *self = MP_OBJ_TO_PTR(self_in);

    switch (request) {
        case MP_STREAM_FLUSH:
            if (self->fp) {
                fflush(self->fp);
            }
            return 0;

        case MP_STREAM_CLOSE:
            if (self->fp) {
                fclose(self->fp);
                self->fp = NULL;
            }
            return 0;

        case MP_STREAM_SEEK: {
            if (!self->fp) {
                *errcode = EBADF;
                return MP_STREAM_ERROR;
            }

            struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)arg;
            if (fseek(self->fp, (long)s->offset, s->whence) != 0) {
                *errcode = errno;
                return MP_STREAM_ERROR;
            }
            s->offset = (mp_off_t)ftell(self->fp);
            return 0;
        }

        default:
            *errcode = EINVAL;
            return MP_STREAM_ERROR;
    }
}

static const mp_stream_p_t mpy_file_stream = {
    .read = mpy_file_read,
    .write = mpy_file_write,
    .ioctl = mpy_file_ioctl,
    .is_text = false,
};

static const mp_rom_map_elem_t mpy_file_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(mpy_file_locals, mpy_file_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mpy_file_type,
    MP_QSTR_file,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, mpy_file_print,
    protocol, &mpy_file_stream,
    locals_dict, &mpy_file_locals
);

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    (void)kwargs;
    const char *path = mp_obj_str_get_str(args[0]);
    const char *mode = (n_args > 1) ? mp_obj_str_get_str(args[1]) : "r";

    if (strncmp(path, SPIFFS_BASE, SPIFFS_BASE_LEN) != 0) {
        mp_raise_OSError(MP_EACCES);
    }

    FILE *fp = fopen(path, mode);
    if (!fp) {
        mp_raise_OSError(errno);
    }

    mpy_file_obj_t *obj = mp_obj_malloc_with_finaliser(mpy_file_obj_t, &mpy_file_type);
    obj->fp = fp;
    return MP_OBJ_FROM_PTR(obj);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);
