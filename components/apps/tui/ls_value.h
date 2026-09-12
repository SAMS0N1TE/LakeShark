/* Named values, so something that is not compiled in can still read them. */

#ifndef LS_VALUE_H
#define LS_VALUE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_VAL_NONE = 0,
    LS_VAL_INT,
    LS_VAL_FLOAT,   /* also the 0..1 form a bar wants */
    LS_VAL_TEXT,
    LS_VAL_BOOL,
} ls_val_kind_t;

typedef struct {
    ls_val_kind_t kind;
    long        i;
    float       f;
    const char *s;      /* must outlive the frame; point at static storage */
} ls_val_t;

typedef bool (*ls_val_fn)(ls_val_t *out);

bool ls_value_publish(const char *path, const char *unit, ls_val_fn fn);

/* False when the path is unknown or the reader declined - a user app renders
   that as `--`, never as zero. A zero that is really "no value" is the exact
   class of quiet lie this project keeps finding. */
bool ls_value_read(const char *path, ls_val_t *out, const char **unit);

int         ls_value_count(void);
const char *ls_value_name(int index);
const char *ls_value_unit(int index);

/* Publishes the built-in set: p25.*, fm.*, adsb.*, rf.*, sys.*. Safe to call
   more than once. */
void ls_value_publish_builtin(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_VALUE_H */
