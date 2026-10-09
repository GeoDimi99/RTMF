#ifndef EXECUTION_MANAGER_H
#define EXECUTION_MANAGER_H

#include <glib.h>
#include "schedule.h"


#define DEFAULT_EXECUTION_MANAGER_NAME "execution-manager"

/*
 * Esegue lo schedule fornito, pianificando l'avvio e la terminazione
 * dei task utilizzando il GMainLoop di GLib.
 */
void em_run_schedule(schedule_t *sched);

#endif // EXECUTION_MANAGER_H