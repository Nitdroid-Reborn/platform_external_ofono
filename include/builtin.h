extern struct ofono_plugin_desc __ofono_builtin_isimodem;
extern struct ofono_plugin_desc __ofono_builtin_n900;
extern struct ofono_plugin_desc __ofono_builtin_modemconf;

static struct ofono_plugin_desc *__ofono_builtin[] = {
  &__ofono_builtin_modemconf,
  &__ofono_builtin_isimodem,
  &__ofono_builtin_n900,
  NULL
};
