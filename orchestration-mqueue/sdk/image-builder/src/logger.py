import logging
import sys

# Unified log schema shared by every project under projects/jeff/:
#   YYYY-MM-DD HH:MM:SS,mmm [LEVEL] component: message
# LEVEL is one of DEBUG, INFO, WARN, ERROR, PERF.

PERF_LEVEL = 25
logging.addLevelName(PERF_LEVEL, "PERF")
logging.addLevelName(logging.WARNING, "WARN")


def _perf(self, iteration, task_id, start_req_ns, end_req_ns, start_res_ns, end_res_ns, core_id):
    """
    Emit one machine-readable [PERF] line. "message" is always the same
    space-separated key=value fields, in the same order, across every
    project, so downstream analysis scripts can rely on a fixed layout:
      iteration task_id start_req_ns end_req_ns start_res_ns end_res_ns
      q_em_tw_ms t_in_tw_ms q_tw_em_ms total_ms core_id
    The four *_ns timestamps are CLOCK_MONOTONIC nanoseconds.
    """
    q_em_tw_ms = (end_req_ns - start_req_ns) / 1e6
    t_in_tw_ms = (start_res_ns - end_req_ns) / 1e6
    q_tw_em_ms = (end_res_ns - start_res_ns) / 1e6
    total_ms = (end_res_ns - start_req_ns) / 1e6

    self.log(
        PERF_LEVEL,
        f"iteration={iteration} task_id={task_id} "
        f"start_req_ns={start_req_ns} end_req_ns={end_req_ns} "
        f"start_res_ns={start_res_ns} end_res_ns={end_res_ns} "
        f"q_em_tw_ms={q_em_tw_ms:.3f} t_in_tw_ms={t_in_tw_ms:.3f} "
        f"q_tw_em_ms={q_tw_em_ms:.3f} total_ms={total_ms:.3f} core_id={core_id}",
    )


logging.Logger.perf = _perf


def get_logger(name="deploy-manager"):
    """
    Returns a logger that prints:
      YYYY-MM-DD HH:MM:SS,mmm [LEVEL] component: message

    `name` becomes the "component" field and should be a fixed value from
    the shared component vocabulary (e.g. "deploy-manager", "image-builder"),
    not __name__ (which is "__main__" for the entry module when run via
    `python -m`, and would break the fixed component naming).
    """
    logger = logging.getLogger(name)
    if not logger.handlers:
        handler = logging.StreamHandler(sys.stdout)
        formatter = logging.Formatter("%(asctime)s [%(levelname)s] %(name)s: %(message)s")
        handler.setFormatter(formatter)
        logger.addHandler(handler)
        logger.setLevel(logging.DEBUG)
    return logger
