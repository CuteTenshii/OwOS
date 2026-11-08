/* notifysend.c - /bin/notify-send: raise a notification from the shell */
#include "notify.h"
#include "opt.h"
#include "owos.h"

static const struct opt opts[] = {
    {'a', "app", OPT_ARG, 'a', "NAME", "application name shown on the notification"},
    {'s', "subtitle", OPT_ARG, 's', "TEXT", "second line under the title"},
    {'i', "image", OPT_ARG, 'i', "FILE.png", "image to show"},
    {'b', "button", OPT_ARG, 'b', "LABEL:CMD", "add a button that runs CMD (repeatable)"},
};

int main(int argc, char **argv)
{
  struct notification n = {0};
  char blabel[NOTIFY_MAX_BUTTONS][NOTIFY_BLABEL_MAX];
  const char *pos[2] = {0, 0};
  int npos = 0;

  struct optstate st;
  opt_init(&st, argc, argv);
  for (int o; (o = opt_next(&st, opts, OPT_N(opts))) != OPT_DONE;) {
    if (o == 'a') {
      n.app = st.arg;
    } else if (o == 's') {
      n.subtitle = st.arg;
    } else if (o == 'i') {
      n.image = st.arg;
    } else if (o == 'b' && n.nbuttons < NOTIFY_MAX_BUTTONS) {
      /* "Label:command", split on the first ':' */
      const char *s = st.arg;
      int k = n.nbuttons, j = 0;
      while (s[j] && s[j] != ':' && j < NOTIFY_BLABEL_MAX - 1)
        blabel[k][j] = s[j], j++;
      blabel[k][j] = '\0';
      while (s[j] && s[j] != ':')
        j++;
      n.buttons[k].label = blabel[k];
      n.buttons[k].action = s[j] == ':' ? s + j + 1 : 0;
      n.nbuttons++;
    } else if (o == OPT_POS) {
      if (npos < 2)
        pos[npos++] = st.arg;
    } else {
      opt_print_usage("notify-send", "TITLE [BODY]", opts, OPT_N(opts));
      return 2;
    }
  }

  if (npos < 1) {
    opt_print_usage("notify-send", "TITLE [BODY]", opts, OPT_N(opts));
    return 2;
  }
  n.title = pos[0];
  n.body = pos[1];

  if (notify_send(&n) != 0) {
    puts("notify-send: notifyd is not running\n");
    return 1;
  }
  return 0;
}
