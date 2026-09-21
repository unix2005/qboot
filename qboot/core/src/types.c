#include <q/core/types.h>

const char *q_err_str(int rc)
{
    switch (rc) {
    case Q_OK:           return "ok";
    case Q_ERR:          return "error";
    case Q_ERR_NOMEM:    return "out of memory";
    case Q_ERR_INVAL:    return "invalid argument";
    case Q_ERR_NOTFOUND: return "not found";
    case Q_ERR_BUSY:     return "busy";
    case Q_ERR_TIMEOUT:  return "timeout";
    case Q_ERR_IO:       return "io error";
    case Q_ERR_EXIST:    return "already exists";
    default:             return "unknown";
    }
}
