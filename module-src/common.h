#ifndef __MODULE_SRC_COMMON_H
#define __MODULE_SRC_COMMON_H
#define mpr_warn(args...) pr_warn("as_sys: " args)
#define mpr_info(args...) pr_info("as_sys: " args)
#define mpr_err(args...)  pr_err("as_sys: " args)
#endif
