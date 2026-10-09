/* libasound, loaded only when something wants a sound device.
 *
 * pjmedia's ALSA backend is linked into a Linux release build, and on its
 * own that would make libasound.so.2 a hard requirement: no ALSA, no
 * datamodem, even though only --speaker ever plays a sound. So the release
 * build leaves libasound off the link line and this file defines the ALSA
 * functions pjmedia calls instead. Each forwards to the real libasound once
 * dm_alsa_load() has dlopen()ed it; until then ALSA lists no devices and
 * opens nothing, so pjmedia starts with no sound device - and, as a bonus,
 * without probing every PCM ALSA knows of at startup.
 *
 * The prototypes are alsa-lib's own: <alsa/asoundlib.h> is included, so the
 * compiler holds each wrapper to its declaration. */
#include "datamodem/alsa_shim.h"

#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>

/* The device list: wrapped by hand, below. */
#define DM_ALSA_HINT_FNS(X)                                                                                        \
    X(int, snd_device_name_hint, (int card, const char *iface, void ***hints), (card, iface, hints), -ENODEV)     \
    X(int, snd_device_name_free_hint, (void **hints), (hints), 0)

/* X(return type, name, parameters, arguments, result while unloaded) */
#define DM_ALSA_FNS(X)                                                                                             \
    X(char *, snd_device_name_get_hint, (const void *hint, const char *id), (hint, id), NULL)                      \
    X(int, snd_lib_error_set_handler, (snd_lib_error_handler_t handler), (handler), 0)                             \
    X(const char *, snd_strerror, (int errnum), (errnum), "ALSA (libasound.so.2) is not loaded")                  \
    X(int, snd_mixer_open, (snd_mixer_t **mixer, int mode), (mixer, mode), -ENODEV)                                \
    X(int, snd_mixer_close, (snd_mixer_t *mixer), (mixer), -ENODEV)                                                \
    X(int, snd_mixer_attach, (snd_mixer_t *mixer, const char *name), (mixer, name), -ENODEV)                       \
    X(int, snd_mixer_load, (snd_mixer_t *mixer), (mixer), -ENODEV)                                                 \
    X(snd_mixer_elem_t *, snd_mixer_first_elem, (snd_mixer_t *mixer), (mixer), NULL)                               \
    X(snd_mixer_elem_t *, snd_mixer_elem_next, (snd_mixer_elem_t *elem), (elem), NULL)                             \
    X(snd_mixer_elem_t *, snd_mixer_find_selem, (snd_mixer_t *mixer, const snd_mixer_selem_id_t *id),              \
      (mixer, id), NULL)                                                                                           \
    X(int, snd_mixer_selem_register,                                                                               \
      (snd_mixer_t *mixer, struct snd_mixer_selem_regopt *options, snd_mixer_class_t **classp),                    \
      (mixer, options, classp), -ENODEV)                                                                           \
    X(const char *, snd_mixer_selem_get_name, (snd_mixer_elem_t *elem), (elem), NULL)                              \
    X(int, snd_mixer_selem_is_active, (snd_mixer_elem_t *elem), (elem), 0)                                         \
    X(int, snd_mixer_selem_has_playback_volume, (snd_mixer_elem_t *elem), (elem), 0)                               \
    X(int, snd_mixer_selem_has_capture_volume, (snd_mixer_elem_t *elem), (elem), 0)                                \
    X(int, snd_mixer_selem_get_playback_volume_range, (snd_mixer_elem_t *elem, long *min, long *max),              \
      (elem, min, max), -ENODEV)                                                                                   \
    X(int, snd_mixer_selem_get_capture_volume_range, (snd_mixer_elem_t *elem, long *min, long *max),               \
      (elem, min, max), -ENODEV)                                                                                   \
    X(int, snd_mixer_selem_set_playback_volume_all, (snd_mixer_elem_t *elem, long value), (elem, value), -ENODEV)  \
    X(int, snd_mixer_selem_set_capture_volume_all, (snd_mixer_elem_t *elem, long value), (elem, value), -ENODEV)   \
    X(size_t, snd_mixer_selem_id_sizeof, (void), (), 0)                                                            \
    X(int, snd_pcm_open, (snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode),                   \
      (pcm, name, stream, mode), -ENODEV)                                                                          \
    X(int, snd_pcm_close, (snd_pcm_t *pcm), (pcm), -ENODEV)                                                        \
    X(int, snd_pcm_drop, (snd_pcm_t *pcm), (pcm), -ENODEV)                                                         \
    X(int, snd_pcm_prepare, (snd_pcm_t *pcm), (pcm), -ENODEV)                                                      \
    X(snd_pcm_sframes_t, snd_pcm_readi, (snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t size),                    \
      (pcm, buffer, size), -ENODEV)                                                                                \
    X(snd_pcm_sframes_t, snd_pcm_writei, (snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t size),             \
      (pcm, buffer, size), -ENODEV)                                                                                \
    X(int, snd_pcm_hw_params, (snd_pcm_t *pcm, snd_pcm_hw_params_t *params), (pcm, params), -ENODEV)               \
    X(size_t, snd_pcm_hw_params_sizeof, (void), (), 0)                                                             \
    X(int, snd_pcm_hw_params_any, (snd_pcm_t *pcm, snd_pcm_hw_params_t *params), (pcm, params), -ENODEV)           \
    X(int, snd_pcm_hw_params_set_access, (snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_access_t _access),  \
      (pcm, params, _access), -ENODEV)                                                                             \
    X(int, snd_pcm_hw_params_set_format, (snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_t val),      \
      (pcm, params, val), -ENODEV)                                                                                 \
    X(int, snd_pcm_hw_params_set_channels, (snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val),        \
      (pcm, params, val), -ENODEV)                                                                                 \
    X(int, snd_pcm_hw_params_set_rate_near,                                                                        \
      (snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int *val, int *dir), (pcm, params, val, dir), -ENODEV) \
    X(int, snd_pcm_hw_params_set_period_size_near,                                                                 \
      (snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val, int *dir), (pcm, params, val, dir),    \
      -ENODEV)                                                                                                     \
    X(int, snd_pcm_hw_params_set_buffer_size_near,                                                                 \
      (snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t *val), (pcm, params, val), -ENODEV)

/* Called for their effect alone. */
#define DM_ALSA_VOID_FNS(X)                                                                                        \
    X(snd_mixer_selem_id_set_name, (snd_mixer_selem_id_t *obj, const char *val), (obj, val))                       \
    X(snd_mixer_selem_id_set_index, (snd_mixer_selem_id_t *obj, unsigned int val), (obj, val))

#define DM_ALSA_PTR(ret, name, params, args, fail) ret(*name) params;
#define DM_ALSA_VOID_PTR(name, params, args) void(*name) params;

/* Written once, by dm_alsa_load() on the main thread before pjsua starts;
 * only read after that. */
static struct
{
    void *lib;
    DM_ALSA_HINT_FNS(DM_ALSA_PTR)
    DM_ALSA_FNS(DM_ALSA_PTR)
    DM_ALSA_VOID_FNS(DM_ALSA_VOID_PTR)
} alsa;

bool dm_alsa_load(void)
{
    void *lib;

    if (alsa.lib != NULL)
        return true;
    lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_GLOBAL);
    if (lib == NULL)
        return false;
#define DM_ALSA_RESOLVE(ret, name, params, args, fail)                                                             \
    if ((*(void **) &alsa.name = dlsym(lib, #name)) == NULL)                                                       \
        goto missing;
#define DM_ALSA_VOID_RESOLVE(name, params, args) DM_ALSA_RESOLVE(void, name, params, args, 0)
    DM_ALSA_HINT_FNS(DM_ALSA_RESOLVE)
    DM_ALSA_FNS(DM_ALSA_RESOLVE)
    DM_ALSA_VOID_FNS(DM_ALSA_VOID_RESOLVE)
    alsa.lib = lib;
    return true;

missing:
    /* A libasound too old for pjmedia: as good as none. */
    dlclose(lib);
    return false;
}

#define DM_ALSA_WRAP(ret, name, params, args, fail)                                                                \
    ret name params                                                                                                \
    {                                                                                                              \
        return (alsa.lib != NULL) ? alsa.name args : (fail);                                                       \
    }
#define DM_ALSA_VOID_WRAP(name, params, args)                                                                      \
    void name params                                                                                               \
    {                                                                                                              \
        if (alsa.lib != NULL)                                                                                      \
            alsa.name args;                                                                                        \
    }

DM_ALSA_FNS(DM_ALSA_WRAP)
DM_ALSA_VOID_FNS(DM_ALSA_VOID_WRAP)

/* The device list is the one place "not loaded" can't be an error: pjmedia
 * would take that for a broken sound system and fail to start at all. An
 * empty list it takes for a machine with no sound card, and carries on. */
static void *no_hints[] = {NULL};

int snd_device_name_hint(int card, const char *iface, void ***hints)
{
    if (alsa.lib == NULL)
    {
        *hints = no_hints;
        return 0;
    }
    return alsa.snd_device_name_hint(card, iface, hints);
}

int snd_device_name_free_hint(void **hints)
{
    if (hints == no_hints)
        return 0;
    return alsa.snd_device_name_free_hint(hints);
}
