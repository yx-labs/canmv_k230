/* POSIX UAC1 PCM binding. USB URBs and transfer ownership stay in RT-Smart. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "pcm_filter.h"
#include "uac1_pcm.h"

typedef struct {
    mp_obj_base_t base;
    int fd;
    bool capture;
    unsigned channels;
    struct pcm_fir filter;
} pcm_obj_t;

static void raise_errno(void)
{
    mp_raise_OSError(errno ? errno : EIO);
}

static pcm_obj_t *checked(mp_obj_t self)
{
    pcm_obj_t *p = MP_OBJ_TO_PTR(self);
    if (p->fd < 0) mp_raise_OSError(EBADF);
    return p;
}

static mp_obj_t u32_tuple(const uint32_t *values, size_t n)
{
    mp_obj_t result[12];
    for (size_t i = 0; i < n; ++i) result[i] = mp_obj_new_int_from_uint(values[i]);
    return mp_obj_new_tuple(n, result);
}

static mp_obj_t capabilities(void)
{
    uint32_t values[8] = {0};
    int fd = open("/dev/uac0i", O_RDONLY | O_NONBLOCK);
    if (fd < 0) raise_errno();
    int ret = ioctl(fd, UAC1_PCM_CAPS, values);
    int saved = errno;
    close(fd);
    if (ret < 0) mp_raise_OSError(saved);
    if (values[0] != 1) mp_raise_OSError(EPROTO);
    return u32_tuple(values, 8);
}
static MP_DEFINE_CONST_FUN_OBJ_0(caps_obj, capabilities);

static mp_obj_t pcm_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw,
                        const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    const char *dir = mp_obj_str_get_str(args[0]);
    bool capture = strcmp(dir, "capture") == 0;
    if (!capture && strcmp(dir, "playback") != 0)
        mp_raise_ValueError(MP_ERROR_TEXT("choose capture or playback"));
    pcm_obj_t *p = m_new_obj_with_finaliser(pcm_obj_t);
    p->base.type = type;
    p->fd = -1;
    p->capture = capture;
    memset(&p->filter, 0, sizeof(p->filter));
    int fd = open(capture ? "/dev/uac0c" : "/dev/uac0p",
                  (capture ? O_RDONLY : O_WRONLY) | O_NONBLOCK);
    if (fd < 0) raise_errno();
    uint32_t v[12] = {0};
    int ret = ioctl(fd, UAC1_PCM_INFO, v);
    if (ret < 0 || v[0] != 1 || !v[1] || v[2] != 48000 ||
        (v[3] != 1 && v[3] != 2) || v[4] != 16) {
        int err = ret < 0 ? errno : ENOTSUP;
        close(fd);
        mp_raise_OSError(err);
    }
    p->fd = fd;
    p->channels = v[3];
    return MP_OBJ_FROM_PTR(p);
}

static mp_obj_t pcm_close(mp_obj_t self)
{
    pcm_obj_t *p = MP_OBJ_TO_PTR(self);
    if (p->fd >= 0) {
        int fd = p->fd;
        p->fd = -1;
        if (close(fd) < 0) raise_errno();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(close_obj, pcm_close);

static mp_obj_t pcm_info(mp_obj_t self)
{
    pcm_obj_t *p = checked(self);
    uint32_t v[12] = {0};
    if (ioctl(p->fd, UAC1_PCM_INFO, v) < 0) raise_errno();
    return u32_tuple(v, 12);
}
static MP_DEFINE_CONST_FUN_OBJ_1(info_obj, pcm_info);

static mp_obj_t pcm_drop(mp_obj_t self)
{
    pcm_obj_t *p = checked(self);
    if (ioctl(p->fd, UAC1_PCM_DROP, 0) < 0) raise_errno();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(drop_obj, pcm_drop);

static mp_obj_t transfer(mp_obj_t self, mp_obj_t data, bool capture)
{
    pcm_obj_t *p = checked(self);
    if (capture != p->capture) mp_raise_OSError(EBADF);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data, &buf, capture ? MP_BUFFER_WRITE : MP_BUFFER_READ);
    if (buf.len % (2 * p->channels)) mp_raise_ValueError(MP_ERROR_TEXT("unaligned native PCM"));
    /* One native request per call. Larger buffers get a normal short count:
     * RT-Smart's LWP bridge stages at most 64 KiB before the next copy. */
    size_t count = buf.len < 65536 ? buf.len : 65536;
    ssize_t n = capture ? read(p->fd, buf.buf, count) : write(p->fd, buf.buf, count);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MP_OBJ_NEW_SMALL_INT(0);
        raise_errno();
    }
    if ((size_t)n > count || n % (2 * p->channels)) mp_raise_OSError(EPROTO);
    return mp_obj_new_int(n);
}

static mp_obj_t pcm_read(mp_obj_t self, mp_obj_t buffer)
{
    return transfer(self, buffer, true);
}
static MP_DEFINE_CONST_FUN_OBJ_2(read_obj, pcm_read);

static mp_obj_t pcm_write(mp_obj_t self, mp_obj_t data)
{
    return transfer(self, data, false);
}
static MP_DEFINE_CONST_FUN_OBJ_2(write_obj, pcm_write);

static void distinct_buffers(mp_obj_t src_in, mp_obj_t dst_in, mp_buffer_info_t *src,
                             mp_buffer_info_t *dst)
{
    mp_get_buffer_raise(src_in, src, MP_BUFFER_READ);
    mp_get_buffer_raise(dst_in, dst, MP_BUFFER_WRITE);
    if (src->len > 65536 || dst->len > 65536)
        mp_raise_ValueError(MP_ERROR_TEXT("PCM chunk too large"));
    uintptr_t s = (uintptr_t)src->buf, d = (uintptr_t)dst->buf;
    if (s < d + dst->len && d < s + src->len)
        mp_raise_ValueError(MP_ERROR_TEXT("overlapping PCM buffers"));
}

static mp_obj_t pcm_decode(mp_obj_t self, mp_obj_t input, mp_obj_t output)
{
    pcm_obj_t *p = checked(self);
    if (!p->capture) mp_raise_OSError(EBADF);
    mp_buffer_info_t src, dst;
    distinct_buffers(input, output, &src, &dst);
    unsigned stride = 2 * p->channels;
    size_t frames = src.len / stride;
    if (src.len % stride || ((frames + p->filter.phase) / 3) * 2 > dst.len)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid capture PCM buffers"));
    size_t n = pcm_down3(&p->filter, src.buf, frames, p->channels, dst.buf);
    return mp_obj_new_int_from_uint(n * 2);
}
static MP_DEFINE_CONST_FUN_OBJ_3(decode_obj, pcm_decode);

static mp_obj_t pcm_encode(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    pcm_obj_t *p = checked(args[0]);
    if (p->capture) mp_raise_OSError(EBADF);
    mp_buffer_info_t src, dst;
    distinct_buffers(args[1], args[2], &src, &dst);
    mp_int_t volume = mp_obj_get_int(args[3]);
    size_t bytes = src.len * 3 * p->channels;
    if (src.len % 2 || bytes > dst.len || volume < 0 || volume > 100)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid playback PCM buffers/volume"));
    pcm_up3(&p->filter, src.buf, src.len / 2, p->channels, (unsigned)volume, dst.buf);
    return mp_obj_new_int_from_uint(bytes);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(encode_obj, 4, 4, pcm_encode);

static const mp_rom_map_elem_t pcm_methods[] = {
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&info_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&write_obj) },
    { MP_ROM_QSTR(MP_QSTR_decode_into), MP_ROM_PTR(&decode_obj) },
    { MP_ROM_QSTR(MP_QSTR_encode_into), MP_ROM_PTR(&encode_obj) },
    { MP_ROM_QSTR(MP_QSTR_drop), MP_ROM_PTR(&drop_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&close_obj) },
};
static MP_DEFINE_CONST_DICT(pcm_dict, pcm_methods);
static MP_DEFINE_CONST_OBJ_TYPE(pcm_type, MP_QSTR_PCM, MP_TYPE_FLAG_NONE,
    make_new, pcm_new, locals_dict, &pcm_dict);

static const mp_rom_map_elem_t uac_globals[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__uac1) },
    { MP_ROM_QSTR(MP_QSTR_capabilities), MP_ROM_PTR(&caps_obj) },
    { MP_ROM_QSTR(MP_QSTR_PCM), MP_ROM_PTR(&pcm_type) },
};
static MP_DEFINE_CONST_DICT(uac_globals_dict, uac_globals);
const mp_obj_module_t canmv_uac1_module = {
    .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&uac_globals_dict
};
MP_REGISTER_MODULE(MP_QSTR__uac1, canmv_uac1_module);
