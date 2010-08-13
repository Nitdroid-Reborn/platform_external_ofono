extern struct ofono_plugin_desc __ofono_builtin_isimodem;
extern struct ofono_plugin_desc __ofono_builtin_modemconf;

static struct ofono_plugin_desc *__ofono_builtin[] = {
  &__ofono_builtin_modemconf,
  &__ofono_builtin_isimodem,
  NULL
};
