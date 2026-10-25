#include <stdlib.h>
/* Unlinked negative-control object: the audit must reject this dependency. */
void td_forbidden_callback(void *pointer) { free(pointer); }
