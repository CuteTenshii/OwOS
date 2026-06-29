/* notifysend.c — /bin/notify-send: raise a notification from the shell.
 *
 *   notify-send <title> [body]
 *   notify-send -i <image.png> <title> [body]
 *   notify-send -a <app> -b "Label:command" <title> [body]
 *
 * -i sets the thumbnail image, -a the sending app's name, and each -b adds an
 * action button "Label:command" (the command is spawned when the button is
 * clicked). The rest is the title and an optional body. */
#include "owos.h"
#include "notify.h"

static void usage(void)
{
	puts("usage: notify-send [-a app] [-s subtitle] [-i image.png] [-b label:cmd] <title> [body]\n");
}

int main(int argc, char **argv)
{
	struct notification n = { 0 };
	char blabel[NOTIFY_MAX_BUTTONS][NOTIFY_BLABEL_MAX];
	const char *pos[2] = { 0, 0 };
	int npos = 0;

	for (int i = 1; i < argc; i++) {
		if (streq(argv[i], "-a") && i + 1 < argc) {
			n.app = argv[++i];
		} else if (streq(argv[i], "-s") && i + 1 < argc) {
			n.subtitle = argv[++i];
		} else if (streq(argv[i], "-i") && i + 1 < argc) {
			n.image = argv[++i];
		} else if (streq(argv[i], "-b") && i + 1 < argc &&
		           n.nbuttons < NOTIFY_MAX_BUTTONS) {
			/* "Label:command" — split on the first ':'. */
			const char *s = argv[++i];
			int k = n.nbuttons, j = 0;
			while (s[j] && s[j] != ':' && j < NOTIFY_BLABEL_MAX - 1)
				blabel[k][j] = s[j], j++;
			blabel[k][j] = '\0';
			while (s[j] && s[j] != ':') j++;
			n.buttons[k].label = blabel[k];
			n.buttons[k].action = s[j] == ':' ? s + j + 1 : 0;
			n.nbuttons++;
		} else if (argv[i][0] == '-' && argv[i][1]) {
			usage();
			return 2;
		} else if (npos < 2) {
			pos[npos++] = argv[i];
		}
	}

	if (npos < 1) { usage(); return 2; }
	n.title = pos[0];
	n.body = pos[1];

	if (notify_send(&n) != 0) {
		puts("notify-send: notifyd is not running\n");
		return 1;
	}
	return 0;
}
