
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <ncurses.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <locale.h>
#include <wchar.h>

#define USER_AGENT \
"Mozilla/5.0 (Windows NT 10.0; Win64; x64) " \
"AppleWebKit/537.36 (KHTML, like Gecko) " \
"Chrome/131.0.0.0 Safari/537.36"

#define RECONNECT_DELAY 1
#define NETWORK_TIMEOUT 5
#define STALL_TIMEOUT 5
#define IPC_INTERVAL 1

#define MIN_WIDTH 72
#define MIN_HEIGHT 30

typedef struct {
    const char *name;
    const char *url;
    int fmplay;
} Station;

/* Radio stations */

static const Station stations[] = {
    {
        "Родники",
        "https://reg.fmplay.ru:8000/rodniki-24.aac",
        1
    },
    {
        "Дорожное радио",
        "https://fed.fmplay.ru:8000/dorozhnoe-24.aac",
        1
    },
    {
        "Наше Радио",
        "https://fed.fmplay.ru:8000/nashe-24.aac",
        1
    },
    {
        "Наше Радио 2.0",
        "https://fed.fmplay.ru:8000/nashe2-24.aac",
        1
    },
    {
        "Радио Родных Дорог",
        "https://fed.fmplay.ru:8000/rrd-24.aac",
        1
    },
    {
        "Питер FM",
        "https://fed.fmplay.ru:8000/piter-24.aac",
        1
    },
    {
        "32 Радио",
        "https://reg.fmplay.ru:8000/32radio-24.aac",
        1
    },
    {
        "Искатель",
        "https://fed.fmplay.ru:8000/iskatel-24.aac",
        1
    },
    {
        "Радио Ангелов",
        "https://fed.fmplay.ru:8000/angelsradio-24.aac",
        1
    }
};

#define STATION_COUNT \
(sizeof(stations) / sizeof(stations[0]))

/* Global state */

static pid_t player_pid = -1;

static volatile sig_atomic_t running = 1;

static int selected_station = 0;
static int playing_station = -1;

static int playing = 0;
static int use_colors = 0;
static int layout_ok = 0;
static int volume = 30;
static int last_drawn_volume = -1;


static time_t next_restart = 0;
static time_t playback_started = 0;
static time_t playback_elapsed = 0;

static time_t last_progress_time = 0;
static time_t last_ipc_check = 0;
static time_t last_timer_value = -1;
static time_t last_clock_value = (time_t)-1;

static double last_time_pos = -1.0;

static char ipc_socket_path[108];

/* Signal handler */

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* Timer formatting */

static void format_time(
    time_t seconds,
    char *buffer,
    size_t size
)
{
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    int secs = seconds % 60;

    snprintf(
        buffer,
        size,
        "%02d:%02d:%02d",
        hours,
        minutes,
        secs
    );
}

/* Stop MPV */

static void stop_player(void)
{
    if (player_pid > 0) {

        kill(player_pid, SIGTERM);

        int status;

        for (int i = 0; i < 20; i++) {

            pid_t result = waitpid(
                player_pid,
                &status,
                WNOHANG
            );

            if (result == player_pid || result == -1) {
                player_pid = -1;
                break;
            }

            struct timespec delay = {
                0, 50000000
            };

            nanosleep(&delay, NULL);
        }

        if (player_pid > 0) {

            kill(player_pid, SIGKILL);

            while (waitpid(player_pid, &status, 0) == -1 &&
                   errno == EINTR) {
            }

            player_pid = -1;
        }
    }

    unlink(ipc_socket_path);
}

/* Start MPV */

static int start_player(int index)
{
    stop_player();

    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0) {

        int fd = open("/dev/null", O_WRONLY);

        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }

        char timeout_arg[] = "--network-timeout=5";
        char volume_arg[32];

        snprintf(volume_arg, sizeof(volume_arg), "--volume=%d", volume);

        char ipc_arg[140];

        snprintf(
            ipc_arg,
            sizeof(ipc_arg),
            "--input-ipc-server=%s",
            ipc_socket_path
        );

        if (stations[index].fmplay) {

            execlp(
                "mpv",
                "mpv",
                "--cache-secs=10",
                "--no-terminal",
                volume_arg,
                "-ao", "coreaudio",
                timeout_arg,
                ipc_arg,
                "--user-agent=" USER_AGENT,
                "--referrer=https://fmplay.ru",
                stations[index].url,
                (char *)NULL
            );

        } else {

            execlp(
                "mpv",
                "mpv",
                "--no-terminal",
                volume_arg,
                "-ao", "coreaudio",
                timeout_arg,
                ipc_arg,
                "--user-agent=" USER_AGENT,
                stations[index].url,
                (char *)NULL
            );
        }

        _exit(127);
    }

    player_pid = pid;

    time_t now = time(NULL);

    last_progress_time = now;
    last_ipc_check = 0;
    last_time_pos = -1.0;

    return 0;
}

/*
 * Query mpv IPC.
 *
 * Returns:
 *  1 = valid time-pos received
 *  0 = no valid response
 */

static int get_mpv_time_pos(double *position)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return 0;

    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;

    snprintf(
        addr.sun_path,
        sizeof(addr.sun_path),
        "%s",
        ipc_socket_path
    );

    if (connect(
        fd,
        (struct sockaddr *)&addr,
        sizeof(addr)
    ) < 0) {

        close(fd);
        return 0;
    }

    const char *request =
        "{\"command\":[\"get_property\",\"time-pos\"],"
        "\"request_id\":1}\n";

    ssize_t sent = write(
        fd,
        request,
        strlen(request)
    );

    if (sent < 0) {
        close(fd);
        return 0;
    }

    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    char buffer[1024];

    size_t used = 0;

    int received = 0;

    while (used < sizeof(buffer) - 1) {

        int result = poll(&pfd, 1, 250);

        if (result <= 0)
            break;

        if (!(pfd.revents & POLLIN))
            break;

        ssize_t n = read(
            fd,
            buffer + used,
            sizeof(buffer) - used - 1
        );

        if (n <= 0)
            break;

        used += n;

        buffer[used] = '\0';

        if (strchr(buffer, '\n')) {
            received = 1;
            break;
        }
    }

    close(fd);

    if (!received)
        return 0;

    char *data = strstr(buffer, "\"data\":");

    if (!data)
        return 0;

    data += 7;

    while (*data == ' ' || *data == '\t')
        data++;

    if (strncmp(data, "null", 4) == 0)
        return 0;

    errno = 0;

    char *end;

    double value = strtod(data, &end);

    if (errno != 0 || end == data)
        return 0;

    *position = value;

    return 1;
}


/* Set MPV volume through its IPC socket. */
static int set_mpv_volume(int value)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return 0;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ipc_socket_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 0;
    }

    char request[128];
    int length = snprintf(
        request,
        sizeof(request),
        "{\"command\":[\"set_property\",\"volume\",%d]}\n",
        value
    );

    if (length <= 0 || (size_t)length >= sizeof(request)) {
        close(fd);
        return 0;
    }

    size_t total_sent = 0;
    while (total_sent < (size_t)length) {
        ssize_t sent = write(
            fd,
            request + total_sent,
            (size_t)length - total_sent
        );

        if (sent < 0 && errno == EINTR)
            continue;

        if (sent <= 0) {
            close(fd);
            return 0;
        }

        total_sent += (size_t)sent;
    }

    close(fd);
    return 1;
}

/* Monitor actual playback progress */

static int check_stream_progress(void)
{
    time_t now = time(NULL);

    if (now - last_ipc_check < IPC_INTERVAL)
        return 0;

    last_ipc_check = now;

    double position;

    if (get_mpv_time_pos(&position)) {

        if (last_time_pos < 0.0 ||
            position > last_time_pos + 0.05 ||
            position < last_time_pos - 0.05) {

            last_progress_time = now;
        }

        last_time_pos = position;
    }

    /* No playback progress for 5 seconds */

    if (now - last_progress_time >= STALL_TIMEOUT)
        return 1;

    return 0;
}

/* Draw static interface */

static void draw_static(void)
{
    int height, width;

    getmaxyx(stdscr, height, width);

    erase();

    layout_ok = 0;

    if (width < MIN_WIDTH || height < MIN_HEIGHT) {

        mvprintw(
            2, 2,
            "Terminal too small. Minimum: %dx%d",
            MIN_WIDTH,
            MIN_HEIGHT
        );

        mvprintw(
            4, 2,
            "Current: %dx%d",
            width,
            height
        );

        mvprintw(
            6, 2,
            "Please resize your terminal."
        );

        wnoutrefresh(stdscr);
        doupdate();

        return;
    }

    const char *title = " FMPLAY.RU Интернет Радио ";

    if (use_colors)
        attron(A_BOLD | COLOR_PAIR(2));
    else
        attron(A_BOLD);

    mvprintw(
        2,
        (width - (int)strlen(title)) / 2,
        "%s",
        title
    );

    attroff(A_BOLD);

    if (use_colors)
        attroff(COLOR_PAIR(2));

    mvhline(4, 2, ACS_HLINE, width - 4);

    mvprintw(6, 4, "Доступные станции (24kbps) :");

    mvhline(height - 9, 2, ACS_HLINE, width - 4);

    if (use_colors)
        attron(A_BOLD | COLOR_PAIR(2));
    else
        attron(A_BOLD);

    mvprintw(height - 8, 4, "Время воспроизведения");

    attroff(A_BOLD);

    if (use_colors)
        attroff(COLOR_PAIR(2));

    mvhline(height - 5, 2, ACS_HLINE, width - 4);

    mvprintw(
        height - 3, 4,
        "ВВЕРХ/ВНИЗ: Станция    ENTER: Играть"
    );

    mvprintw(
        height - 2, 4,
        "LEFT/RIGHT: Громкость    S: Стоп    Q / ESC: Выход"
    );

    layout_ok = 1;

    wnoutrefresh(stdscr);
    doupdate();
}

/*
 * Print UTF-8 text using terminal column width, not byte count.
 * This prevents broken Cyrillic characters and uneven alignment.
 */
static void add_utf8_padded(int y, int x, const char *text, int columns)
{
    wchar_t input[128];
    wchar_t output[256];

    size_t count = mbstowcs(input, text, sizeof(input) / sizeof(input[0]) - 1);

    if (count == (size_t)-1) {
        for (int i = 0; i < columns && i < 255; i++)
            output[i] = L'?';
        int n = columns < 255 ? columns : 255;
        output[n] = L'\0';
        mvaddnwstr(y, x, output, n);
        return;
    }

    input[count] = L'\0';

    int used_columns = 0;
    int out_count = 0;

    for (size_t i = 0; i < count && out_count < 255; i++) {
        int char_width = wcwidth(input[i]);

        if (char_width < 0) {
            if (used_columns + 1 > columns)
                break;
            output[out_count++] = L'?';
            used_columns++;
            continue;
        }

        if (used_columns + char_width > columns)
            break;

        output[out_count++] = input[i];
        used_columns += char_width;
    }

    while (used_columns < columns && out_count < 255) {
        output[out_count++] = L' ';
        used_columns++;
    }

    output[out_count] = L'\0';
    mvaddnwstr(y, x, output, out_count);
}

/* Update one station row */

static void update_station_row(int index)
{
    if (!layout_ok || index < 0 ||
        index >= (int)STATION_COUNT)
        return;

    /* Compact station list: one terminal row per station. */
    int y = 8 + index;

    if (index == selected_station)
        attron(A_REVERSE | A_BOLD);

    mvaddstr(y, 6, index == selected_station ? " > " : "   ");
    add_utf8_padded(y, 9, stations[index].name, 27);
    mvaddch(y, 36, ' ');

    if (index == selected_station)
        attroff(A_REVERSE | A_BOLD);

    move(y, 42);
    clrtoeol();

    if (playing && index == playing_station) {

        if (player_pid > 0) {

            if (use_colors)
                attron(COLOR_PAIR(3) | A_BOLD);
            else
                attron(A_BOLD);

            printw("Проигрывание");

            attroff(A_BOLD);

            if (use_colors)
                attroff(COLOR_PAIR(3));

        } else {

            if (use_colors)
                attron(COLOR_PAIR(4) | A_BOLD);
            else
                attron(A_BOLD);

            printw("ПЕРЕПОДКЛЮЧЕНИЕ");

            attroff(A_BOLD);

            if (use_colors)
                attroff(COLOR_PAIR(4));
        }
    }
}

/* Update station list */

static void update_station_list(void)
{
    if (!layout_ok)
        return;

    for (int i = 0; i < (int)STATION_COUNT; i++)
        update_station_row(i);
}

/* Update timer */

static void update_timer(void)
{
    if (!layout_ok)
        return;

    int height, width;

    getmaxyx(stdscr, height, width);
    (void)width;

    time_t elapsed = 0;

    if (playing && playback_started > 0) {

        elapsed =
            playback_elapsed +
            (time(NULL) - playback_started);

        if (elapsed < 0)
            elapsed = playback_elapsed;
    }

    if (elapsed == last_timer_value)
        return;

    last_timer_value = elapsed;

    char timer[32];

    format_time(
        elapsed,
        timer,
        sizeof(timer)
    );

    move(height - 7, 4);

    attron(A_BOLD);

    printw("%s", timer);

    attroff(A_BOLD);

    clrtoeol();

    wnoutrefresh(stdscr);
    doupdate();
}

/* Draw the volume bar near the bottom of the interface. */
static void update_volume_bar(void)
{
    if (!layout_ok || volume == last_drawn_volume)
        return;

    int height, width;
    getmaxyx(stdscr, height, width);
    (void)width;

    const int y = height - 4;
    const int bar_x = 18;
    const int bar_width = 28;
    int filled = (volume * bar_width + 50) / 100;

    move(y, 4);
    clrtoeol();
    mvaddstr(y, 4, "Громкость:");

    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            if (use_colors)
                attron(COLOR_PAIR(3) | A_BOLD);
            else
                attron(A_BOLD);
            mvaddch(y, bar_x + i, '#');
            attroff(A_BOLD);
            if (use_colors)
                attroff(COLOR_PAIR(3));
        } else {
            mvaddch(y, bar_x + i, '-');
        }
    }

    mvprintw(y, bar_x + bar_width + 2, "%3d%%", volume);
    last_drawn_volume = volume;

    wnoutrefresh(stdscr);
    doupdate();
}

/* Update current local time in the upper-right corner */
static void update_clock(void)
{
    if (!layout_ok)
        return;

    time_t now = time(NULL);

    if (now == last_clock_value)
        return;

    last_clock_value = now;

    struct tm local_time;
    char clock_text[16];

    if (localtime_r(&now, &local_time) == NULL ||
        strftime(clock_text, sizeof(clock_text),
                 "%H:%M:%S", &local_time) == 0) {
        return;
    }

    int height, width;
    getmaxyx(stdscr, height, width);
    (void)height;

    int x = width - (int)strlen(clock_text) - 3;

    if (x < 0)
        return;

    attron(A_BOLD);
    mvaddstr(2, x, clock_text);
    attroff(A_BOLD);

    wnoutrefresh(stdscr);
    doupdate();
}

/* Update bottom status */

static void update_footer(void)
{
    if (!layout_ok)
        return;

    int height, width;

    getmaxyx(stdscr, height, width);

    move(height - 1, 4);
    clrtoeol();

    if (playing && playing_station >= 0) {
        addstr("Текущая станция: ");
        add_utf8_padded(height - 1, 20,
                        stations[playing_station].name,
                        width > 17 ? width - 17 : 0);
    } else {
        addstr("Воспроизведение остановлено");
    }

    wnoutrefresh(stdscr);
    doupdate();
}

/* Initial rendering */

static void draw_interface(void)
{
    last_timer_value = -1;
    last_drawn_volume = -1;

    draw_static();

    if (!layout_ok)
        return;

    update_station_list();
    update_timer();
    update_volume_bar();
    update_clock();
    update_footer();

    wnoutrefresh(stdscr);
    doupdate();
}

/* Main */

int main(void)
{
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    snprintf(
        ipc_socket_path,
        sizeof(ipc_socket_path),
        "/tmp/radio-mpv-%ld.sock",
        (long)getpid()
    );

    setlocale(LC_ALL, "");

    initscr();

    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    curs_set(0);

    timeout(200);

    leaveok(stdscr, TRUE);

    if (has_colors()) {

        use_colors = 1;

        start_color();

        init_pair(2, COLOR_CYAN, COLOR_BLACK);
        init_pair(3, COLOR_GREEN, COLOR_BLACK);
        init_pair(4, COLOR_YELLOW, COLOR_BLACK);
    }

    draw_interface();

    while (running) {

        int state_changed = 0;

        time_t now = time(NULL);

        /* Check MPV process */

        if (playing && player_pid > 0) {

            int status;

            pid_t result = waitpid(
                player_pid,
                &status,
                WNOHANG
            );

            if (result == player_pid || result == -1) {

                player_pid = -1;

                /* MPV exited: reset the playback timer while reconnecting. */
                playback_elapsed = 0;
                playback_started = 0;
                last_timer_value = -1;

                next_restart = now + RECONNECT_DELAY;

                state_changed = 1;
            }
        }

        /*
         * Check stream progress through IPC.
         * If time-pos stops changing for 5 seconds,
         * terminate MPV and reconnect.
         */

        if (playing && player_pid > 0) {

            if (check_stream_progress()) {

                stop_player();

                /* Stream stalled: reset timer before the reconnect attempt. */
                playback_elapsed = 0;
                playback_started = 0;
                last_timer_value = -1;

                next_restart = time(NULL) + RECONNECT_DELAY;

                state_changed = 1;
            }
        }

        /* Automatic infinite reconnection */

        if (playing &&
            player_pid == -1 &&
            time(NULL) >= next_restart) {

            if (start_player(playing_station) == 0) {

                /* A new MPV process means a new playback session. */
                playback_elapsed = 0;
                playback_started = time(NULL);
                last_timer_value = -1;

            } else {

                /* Keep the timer at zero until MPV starts successfully. */
                playback_elapsed = 0;
                playback_started = 0;
                last_timer_value = -1;

                next_restart =
                    time(NULL) + RECONNECT_DELAY;
            }

            state_changed = 1;
        }

        if (state_changed) {

            update_station_row(playing_station);
        }

        /* Keyboard */

        int ch = getch();

        switch (ch) {

            case KEY_UP:
            {
                if (selected_station > 0) {

                    int old = selected_station;

                    selected_station--;

                    update_station_row(old);
                    update_station_row(selected_station);

                    wnoutrefresh(stdscr);
                    doupdate();
                }

                break;
            }

            case KEY_DOWN:
            {
                if (selected_station <
                    (int)STATION_COUNT - 1) {

                    int old = selected_station;

                    selected_station++;

                    update_station_row(old);
                    update_station_row(selected_station);

                    wnoutrefresh(stdscr);
                    doupdate();
                }

                break;
            }

            case KEY_LEFT:
            case KEY_RIGHT:
            {
                int old_volume = volume;

                if (ch == KEY_LEFT)
                    volume -= 5;
                else
                    volume += 5;

                if (volume < 0)
                    volume = 0;
                if (volume > 100)
                    volume = 100;

                if (volume != old_volume) {
                    if (player_pid > 0)
                        set_mpv_volume(volume);

                    update_volume_bar();
                }

                break;
            }

            case '\n':
            case KEY_ENTER:
            {
                int old_station = playing_station;

                playing_station = selected_station;

                if (start_player(playing_station) == 0) {

                    playing = 1;

                    next_restart = 0;

                    playback_elapsed = 0;
                    playback_started = time(NULL);

                } else {

                    playing = 1;

                    next_restart =
                        time(NULL) + RECONNECT_DELAY;

                    playback_elapsed = 0;
                    playback_started = time(NULL);
                }

                last_timer_value = -1;

                if (old_station >= 0)
                    update_station_row(old_station);

                update_station_list();
                update_timer();
                update_footer();

                wnoutrefresh(stdscr);
                doupdate();

                break;
            }

            case 's':
            case 'S':
            {
                int old_station = playing_station;

                stop_player();

                playing = 0;
                playing_station = -1;

                next_restart = 0;

                playback_elapsed = 0;
                playback_started = 0;

                last_timer_value = -1;

                if (old_station >= 0)
                    update_station_row(old_station);

                update_timer();
                update_footer();

                wnoutrefresh(stdscr);
                doupdate();

                break;
            }

            case 'q':
            case 'Q':
            case 27:

                running = 0;

                break;

            case KEY_RESIZE:

                draw_interface();

                break;
        }

        /* Update playback timer and wall clock */

        update_timer();
        update_volume_bar();
        update_clock();
    }

    stop_player();

    endwin();

    return 0;
}
