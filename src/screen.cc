/*
 * screen.cc - Our main object.
 *
 * This file is part of lumail - http://lumail.org/
 *
 * Copyright (c) 2015 by Steve Kemp.  All rights reserved.
 *
 **
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 dated June, 1991, or (at your
 * option) any later version.
 *
 * On Debian GNU/Linux systems, the complete text of version 2 of the GNU
 * General Public License can be found in `/usr/share/common-licenses/GPL-2'
 */

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <pcrecpp.h>

#include "attachment_view.h"
#include "config.h"
#include "colour_string.h"
#include "history.h"
#include "index_view.h"
#include "input_queue.h"
#include "keybinding_view.h"
#include "lua.h"
#include "lua_view.h"
#include "maildir_view.h"
#include "message_view.h"
#include "screen.h"

#include "statuspanel.h"



/*
 * Constructor.
 */
CScreen::CScreen() : Observer(CConfig::instance())
{
}



/*
 * Destructor.  NOP.
 */
CScreen::~CScreen()
{
    teardown();
}


/*
 * Register a view mode.
 *
 * The name will be the name of the mode, as seen by lua, and the
 * implementation will be a class derived from CViewMode.
 */
void CScreen::register_view(std::string name, CViewMode *impl)
{
    m_views[name] = impl;
}


/**
 * Return all registered view-modes.
 */
std::vector<std::string> CScreen::view_modes()
{
    std::vector<std::string> result;

    for (auto it = m_views.begin(); it != m_views.end(); it++)
    {
        result.push_back(it->first);
    }

    std::sort(result.begin(), result.end());
    return (result);
}

/*
 * This method is called when a configuration key changes,
 * via our observer implementation.
 */
void CScreen::update(std::string key_name)
{
    /*
     * If our timeout value has changed then update
     * our loop.
     */
    if (key_name == "global.timeout")
    {
        CConfig *config = CConfig::instance();
        int value       = config->get_integer("global.timeout", 200);
        timeout(value);
    }

    if (key_name == "global.mode")
    {
        /*
         * The value the user set.
         */
        CConfig *config = CConfig::instance();
        std::string nm  = config->get_string("global.mode");

        /*
         * Ensure the new-mode is valid.
         */
        bool found = false;
        std::vector<std::string> modes = view_modes();

        for (auto it = modes.begin(); it != modes.end(); ++it)
        {
            if (nm == (*it))
                found = true;
        }

        if (found == false)
        {
            CStatusPanel *panel = CStatusPanel::instance();
            panel->add_text("Prevented invalid mode change :" + nm);
            config->set("global.mode", "maildir", false);
        }
    }
}


/*
 * Run our event loop.
 */
void CScreen::run_main_loop()
{
    /*
     * Get our timeout period, and set it.
     */
    CConfig *config = CConfig::instance();
    int tout = config->get_integer("global.timeout", 200);

    timeout(tout);

    /*
     * Now we're in our loop.
     */
    m_running = true;

    /*
     * Get the lua-helper.
     */
    CLua *lua = CLua::instance();

    /*
     * Holder for keyboard input.
     */
    int ch;

    /*
     * Input handler.
     */
    CInputQueue *input = CInputQueue::instance();

    while ((m_running) && (ch = input->get_input()))
    {

        /*
         * Clear the screen.
         */
        clear(false);


        /*
         * Get the current global mode.
         */
        CConfig *config  = CConfig::instance();
        std::string mode = config->get_string("global.mode", "maildir");


        /*
         * Get the virtual view class.
         */
        CViewMode *view = m_views[mode];

        /*
         * If the key fetching timed out then call our idle functions.
         */
        if (ch == ERR)
        {
            /*
             * Call the Lua on_idle() function.
             */
            lua->execute("on_idle()");

            /*
             * Call our view-specific on-idle handler.
             */
            if (view)
                view->on_idle();
        }
        else
        {
            /*
             * Convert the key-press to a key-name, which means that
             * "down" will be "KEY_DOWN", for example.
             */
            const char *key = lookup_key(ch);

            if (key != NULL)
                on_keypress(key);
        }


        /*
         * Check if the view has changed (after key handling).
         *
         * This avoids a single redraw of the wrong mode, before we
         * run round the event-loop again and draw in the correct mode.
         */
        std::string new_mode = config->get_string("global.mode", "maildir");

        if (new_mode != mode)
            view = m_views[new_mode];

        /*
         * Update the view.
         */
        if (view)
            view->draw();

        /*
         * Update our panel
         */
        CStatusPanel *instance = CStatusPanel::instance();

        if (! instance->hidden())
            instance->draw();

        /*
         * Update our panel.
         */
        update_panels();
        doupdate();
        refresh();
    }
}


/*
 * Exit our main event-loop
 */
void CScreen::exit_main_loop()
{
    m_running = false;
}


/*
 * Execute a command via `system`.
 */
void CScreen::execute(std::string prog)
{
    int result __attribute__((unused));

    /*
     * Save the current state of the TTY
     */
    refresh();
    def_prog_mode();
    endwin();

    /* Run the command */
    result = system(prog.c_str());

    /*
     * Reset + redraw
     */
    reset_prog_mode();
    refresh();
}

/*
 * Convert "^I" -> "TAB", etc.
 */
const char *CScreen::lookup_key(int c)
{
    if (c == '\n')
        return ("ENTER");

    if (c == '\t')
        return ("TAB");

    if (c == ' ')
        return ("SPACE");

    return (keyname(c));
}


/*
 * Setup the curses/screen.
 */
void CScreen::setup()
{
    /*
     * Setup locale.
     */
    setlocale(LC_CTYPE, "");
    setlocale(LC_ALL, "");

    char e[] = "ESCDELAY=0";
    putenv(e);

    /*
     * Setup ncurses.
     */
    initscr();

    /*
     * Make sure we have colours.
     */
    if (!has_colors() || (start_color() != OK))
    {
        endwin();
        std::cerr << "Missing colour support" << std::endl;
        exit(1);
    }

    /* Initialize curses */
    keypad(stdscr, TRUE);
    crmode();
    noecho();
    curs_set(0);

    /*
     * Get our timeout period, and set it.
     */
    CConfig *config = CConfig::instance();
    int tout = config->get_integer("global.timeout", 200);

    timeout(tout);
    use_default_colors();


    /* Initialize all the colors */
    init_pair(1, COLOR_WHITE, -1);
    m_colours[ "white" ] = 1;

    init_pair(2, COLOR_RED, -1);
    m_colours[ "red" ] = 2;

    init_pair(3, COLOR_BLUE, -1);
    m_colours[ "blue" ] = 3;

    init_pair(4, COLOR_GREEN, -1);
    m_colours[ "green" ] = 4;

    init_pair(5, COLOR_CYAN, -1);
    m_colours[ "cyan" ] = 5;

    init_pair(6, COLOR_MAGENTA, -1);
    m_colours[ "magenta" ] = 6;

    init_pair(7, COLOR_YELLOW, -1);
    m_colours[ "yellow" ] = 7;

    init_pair(8, COLOR_BLACK, COLOR_WHITE);
    m_colours[ "black" ] = 8;

    CStatusPanel *panel = CStatusPanel::instance();
    panel->init(6);
}


/*
 * Shutdown curses.
 */
void CScreen::teardown()
{
    /*
     * Remove old panel/window - in the correct order.
     */

    endwin();
}


/*
 * Clear the whole screen by printing lines of blanks.
 */
void CScreen::clear(bool refresh_screen)
{
    int width = CScreen::width();
    int height = CScreen::height();

    CStatusPanel *panel = CStatusPanel::instance();

    if (panel->hidden() == false)
        height -= panel->height();

    std::string blank = "";

    while ((int) blank.size() < width)
        blank += " ";

    for (int i = 0; i <= height; i++)
    {
        mvprintw(i, 0, "%s", blank.c_str());
    }

    if (refresh_screen)
    {
        update_panels();
        doupdate();
        refresh();
    }
}


/*
 * Return the height of the screen.
 */
int CScreen::height()
{
    struct winsize w;
    ioctl(0, TIOCGWINSZ, &w);
    return (w.ws_row);
}


/*
 * Delay for the given period.
 */
void CScreen::sleep(int seconds)
{
    ::sleep(seconds);
}

/*
 * Return the width of the screen.
 */
int CScreen::width()
{
    struct winsize w;
    ioctl(0, TIOCGWINSZ, &w);
    return (w.ws_col);
}


/*
 * Choose a single item from a small selection.
 *
 * (This is used to resolve ambiguity in TAB-completion.)
 */
std::string CScreen::choose_string(std::vector<std::string> choices)
{
    /*
     * We don't need to resolve ambiguity unless there is more than
     * one choice to choose from.
     */
    assert(choices.size() > 0);

    /*
     * Find longest/widest entry.
     */
    size_t max = 0;

    for (std::string choice : choices)
    {
        if (choice.size() > max)
            max = choice.size();
    }

    /*
     * Get the dimensions.
     */
    int height = CScreen::height() - 4;
    int width = CScreen::width() - 4;
    size_t cols = 1;

    WINDOW *childwin = newwin(height, width, 2, 2);
    box(childwin, 0, 0);

    /*
     * How many columns to draw?
     */
    for (int i = 1; i < 12; i++)
    {
        if (max < (size_t(width) / i))
            cols = i;
    }

    /*
     * We'll be careful to not draw more columns than we have items.
     */
    if (cols > choices.size())
        cols = choices.size();


    int selected  = 0;
    bool done     = false;
    int col_width = width / cols;

    timeout(0);

    while (!done)
    {
        refresh();

        int count = 0;

        /*
         * Drawing of each item.
         */
        int x = 0;
        int y = 2;

        for (std::string choice : choices)
        {

            /*
             * Calculate the column.
             */
            x = 2 + ((count % cols) * col_width);
            y = 1 + (count / cols);


            if (count == selected)
                wattron(childwin, A_UNDERLINE | A_STANDOUT);
            else
                wattrset(childwin, A_NORMAL);

            mvwaddstr(childwin, y, x,  choice.c_str());
            count += 1;
        }

        wrefresh(childwin);

        /*
         * Read input from the queue / keyboard.
         */
        CInputQueue *input = CInputQueue::instance();
        int c = input->get_input();

        if (c == '\n')
            done = true;

        if (c == 27)
        {
            delwin(childwin);
            ::clear();
            /*
             * Get our timeout period, and set it.
             */
            CConfig *config = CConfig::instance();
            int tout = config->get_integer("global.timeout", 200);

            timeout(tout);
            return "";
        }

        if (c == '\t')
        {
            selected += 1;

            if (selected >= (int)choices.size())
                selected = 0;
        }

        if (c == KEY_RIGHT)
        {
            selected += 1;

            if (selected >= (int)choices.size())
                selected = 0;
        }

        if (c == KEY_LEFT)
        {
            selected -= 1;

            if (selected < 0)
                selected = (int)(choices.size() - 1);

        }
    }

    delwin(childwin);
    ::clear();
    /*
     * Get our timeout period, and set it.
     */
    CConfig *config = CConfig::instance();
    int tout = config->get_integer("global.timeout", 200);

    timeout(tout);
    return (choices.at(selected));
}


/*
 * Read a line of input via the status-line.
 */
std::string CScreen::get_line(std::string prompt, std::string input)
{
    std::string buffer;


    int old_curs = curs_set(1);
    int pos = 0;
    int x, y;
    int orig_x, orig_y;

    if (! input.empty())
    {
        buffer = input;
        pos = input.size();
    }

    /*
     * Gain access to any past history.
     */
    CHistory *history  = CHistory::instance();
    int history_offset = history->size();

    /*
     * Get the cursor position
     */
    getyx(stdscr, orig_y, orig_x);

    /*
     * Determine where to move the cursor to.  If the panel is visible it'll
     * be above that.
     */
    x = 0;
    y = height() - 1;

    CStatusPanel *panel = CStatusPanel::instance();

    if (!panel->hidden())
    {
        y -= panel->height();
    }

    /*
     * Draw the prompt, and make sure we place the cursor at a suitable spot.
     */
    mvaddnstr(y, x, prompt.c_str(), prompt.length());
    x += prompt.length();

    /*
     * Get the mode so we can update the display mid-input.
     */
    CConfig *config   = CConfig::instance();
    std::string mode  = config->get_string("global.mode", "maildir");

    CViewMode *view = m_views[mode];

    /*
     * We'll call our idle function too.
     */
    CLua *lua = CLua::instance();

    while (true)
    {
        int  c;

        /*
         * Redraw the main display.
         */
        if (view)
            view->draw();

        /*
         * Run our on_idle() functions.
         */
        lua->execute("on_idle()");

        if (view)
            view->on_idle();

        /*
         * Update our panel
         */
        CStatusPanel *instance = CStatusPanel::instance();

        if (! instance->hidden())
            instance->draw();

        update_panels();
        doupdate();
        refresh();

        mvaddnstr(y, 0, prompt.c_str(), prompt.length());
        mvaddnstr(y, x, buffer.c_str(), buffer.size());

        /*
          * Clear the line- the "-2" comes from the size of the prompt.
          */
        for (int padding = buffer.size(); padding < (width() - 1 - (int)prompt.length()); padding++)
            printw(" ");

        /*
          * Move the cursor
          */
        move(y, x + pos);

        /*
         * Read input from the queue / keyboard.
         */
        CInputQueue *input = CInputQueue::instance();
        c = input->get_input();

        /*
          * Ropy input-handler.
          */
        if (c == KEY_ENTER || c == '\n' || c == '\r')
        {
            break;
        }
        else if (c == 1)	/* ctrl-a : beginning of line */
        {
            pos = 0;
        }
        else if (c == 5)	/* ctrl-e: end of line */
        {
            pos = buffer.size();
        }
        else if (c == 11)	/* ctrl-k: kill to end of line */
        {
            /*
                 * Kill the buffer from the current position onwards.
                 */
            buffer = buffer.substr(0, pos);
        }
        else if ((c == 2) ||	/* ctrl-b : back char */
                 (c == KEY_LEFT))
        {
            if (pos > 0)
                pos -= 1;
        }
        else if ((c == 6) ||	/* ctrl-f: forward char */
                 (c == KEY_RIGHT))
        {
            if (pos < (int) buffer.size())
                pos += 1;
        }
        else if (c == KEY_UP)
        {
            history_offset -= 1;

            if (history_offset >= 0)
            {
                buffer = history->at(history_offset);
                pos    = buffer.size();
            }
            else
            {
                history_offset = 0;
            }
        }
        else if (c == KEY_DOWN)
        {
            history_offset += 1;

            if (history_offset < history->size())
            {
                buffer = history->at(history_offset);
                pos    = buffer.size();
            }
            else
            {
                history_offset = history->size();
            }
        }
        else if (c == KEY_BACKSPACE)
        {
            if (pos > 0)
            {
                buffer.erase(pos - 1, 1);
                pos -= 1;
            }
        }
        else if (c == 4)	/* ctrl+d */
        {
            /*
             * Remove the character after the point.
             */
            if (pos < (int) buffer.size())
            {
                buffer.erase(pos, 1);
            }
        }
        else if ((c == '\t') && (! buffer.empty()))      /* TAB-completion */
        {
            /*
             * We're going to find the token to complete against
             * by searching backwards for a position to start from.
             *
             * This string comes from lua, and includes things like: ( " ' space
             *
             */
            size_t toke = buffer.find_last_of("(\"' ", pos);

            std::string prefix = "";
            std::string token  = buffer;

            /*
             * If we found one of the split-characters then we have
             * a token to complete, and the prefix to ignore.
             *
             * If we didn't then the prefix is empty, and the buffer is
             * the token; i.e. we're completing the sole token on the line.
             *
             * NOTE:  This implies you cannot complete in the middle of a line.
             * Just at the end.  Or start.
             *
             */
            if (toke != std::string::npos)
            {
                prefix = buffer.substr(0, toke + 1);
                token = token.substr(toke + 1);
            }


            /*
             * The token length - because we want to update the cursor position, post-completion.
             */
            int toke_len = token.size();

            /*
             * Get the completions.
             */
            CLua *lua = CLua::instance();
            std::vector<std::string> matches = lua->function2table("on_complete", token);

            if (matches.size() == 0)
            {
                /*
                 * No completion possible.
                 */
                beep();
            }
            else
            {
                /*
                 * Single completion == match.
                 */
                if (matches.size() == 1)
                {
                    buffer = prefix + matches.at(0).c_str();
                    pos += (matches.at(0).size() - toke_len);
                }
                else
                {
                    /*
                     * Disable echoing before showing the menu.
                     */
                    noecho();
                    curs_set(0);

                    /*
                     * Prompt for clarification in the multiple-matches.
                     */
                    std::string choice = choose_string(matches);

                    /*
                     * Reset the cursor.
                     */
                    curs_set(1);
                    echo();

                    /*
                     * If the user did make a specific choice, then use it.
                     */
                    if (! choice.empty())
                    {
                        buffer = prefix + choice.c_str();
                        pos += (choice.size() - toke_len);
                    }
                }
            }
        }
        else if (isprint(c))
        {
            /*
             * Insert the character into the buffer-string.
             */
            buffer.insert(pos, 1, c);
            pos += 1;
        }

    }

    if (old_curs != ERR)
        curs_set(old_curs);

    /*
     * Add the line to the history.
     */
    history->add(buffer);

    return (buffer);
}


/*
 * Show a message and return only a valid keypress from a given set.
 */
std::string CScreen::prompt_chars(std::string prompt, std::string valid)
{
    int orig_x, x;
    int orig_y, y;

    /*
     * Get the cursor position
     */
    getyx(stdscr, orig_y, orig_x);

    /*
     * Ensure we draw a complete line when showing our prompt.
     */
    while ((int)prompt.length() < CScreen::width())
        prompt += " ";

    /*
     * Determine where to move the cursor to.  If the panel is visible it'll
     * be above that.
     */
    x = 0;
    y = height() - 1;

    CStatusPanel *panel = CStatusPanel::instance();

    if (!panel->hidden())
    {
        y -= panel->height();
    }

    /*
     * Get the mode so we can update the display mid-input.
     */
    CConfig *config   = CConfig::instance();
    std::string mode  = config->get_string("global.mode", "maildir");

    CViewMode *view = m_views[mode];

    /*
     * We'll call our idle function too.
     */
    CLua *lua = CLua::instance();



    while (true)
    {
        int  c;

        /*
         * Redraw the main display.
         */
        if (view)
            view->draw();

        /*
         * Run our on_idle() functions.
         */
        lua->execute("on_idle()");

        if (view)
            view->on_idle();

        /*
         * Update our panel
         */
        CStatusPanel *instance = CStatusPanel::instance();

        if (! instance->hidden())
            instance->draw();

        update_panels();
        doupdate();
        refresh();

        mvaddnstr(y, x, prompt.c_str(), prompt.length());

        /*
         * Read input from the queue / keyboard.
         */
        CInputQueue *input = CInputQueue::instance();
        c = input->get_input();

        for (unsigned int i = 0; i < valid.size(); i++)
        {
            if (valid[i] == c)
            {
                std::string result = "x";
                result[0] = c;
                return (result);
            }
        }
    }
}


/*
 * Show a prompt and wait for a single character.
 */
std::string CScreen::get_char(std::string prompt)
{
    int orig_x, x;
    int orig_y, y;

    /*
     * Get the cursor position
     */
    getyx(stdscr, orig_y, orig_x);

    /*
     * Ensure we draw a complete line when showing our prompt.
     */
    while ((int)prompt.length() < CScreen::width())
        prompt += " ";

    /*
     * Determine where to move the cursor to.  If the panel is visible it'll
     * be above that.
     */
    x = 0;
    y = height() - 1;

    CStatusPanel *panel = CStatusPanel::instance();

    if (!panel->hidden())
    {
        y -= panel->height();
    }

    /*
     * Get the mode so we can update the display mid-input.
     */
    CConfig *config  = CConfig::instance();
    std::string mode = config->get_string("global.mode", "maildir");
    CViewMode *view  = m_views[mode];

    /*
     * We'll call our idle function too.
     */
    CLua *lua = CLua::instance();


    while (true)
    {
        int  c;

        /*
         * Redraw the main display.
         */
        if (view)
            view->draw();

        /*
         * Run our on_idle() functions.
         */
        lua->execute("on_idle()");

        if (view)
            view->on_idle();

        /*
         * Update our panel
         */
        CStatusPanel *instance = CStatusPanel::instance();

        if (! instance->hidden())
            instance->draw();

        update_panels();
        doupdate();
        refresh();

        mvaddnstr(y, x, prompt.c_str(), prompt.length());

        /*
         * Read input from the queue / keyboard.
         */
        CInputQueue *input = CInputQueue::instance();
        c = input->get_input();

        if ((c > 0) && (c != KEY_RESIZE))
        {
            std::string out;
            out = lookup_key(c);
            return (out);
        }
    }
}


/*
 * Look up the binding for the named keystroke in our keymap(s).
 *
 * If the result is a string then execute it as a function.
 */
bool CScreen::on_keypress(const char *key)
{
    /*
     * The result of the lookup.
     */
    char *result = NULL;

    /*
     * Get the current global-mode.
     */
    CConfig *config  = CConfig::instance();
    std::string mode = config->get_string("global.mode", "message");

    /*
     * Lookup the keypress in the current-mode-keymap.
     */
    CLua *lua = CLua::instance();
    result = lua->get_nested_table("keymap", mode.c_str(), key);


    /*
     * If that failed then lookup the global keymap.
     *
     * This order ensures you can have a "global" keymap, overridden in just one mode.
     */
    if (result == NULL)
        result = lua->get_nested_table("keymap", "global", key);

    /*
     * If one/other of these lookups resulted in success then we're golden.
     */
    if (result != NULL)
        lua->execute(result);

    /*
     * We succeeded if the result wasn't NULL.
     */
    return (result != NULL);
}


int CScreen::get_colour(std::string name)
{
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    /*
     * If the name is "unread" then remap that to the default configured
     * colour/attribute.
     */
    if (name == "unread")
    {
        CConfig *config = CConfig::instance();
        name = config->get_string("colour.unread", "red");
    }

    /*
     * Does the colour contain "|ATTRIBUTE1|ATTRIBUTE2|..N"?
     */
    std::size_t pipe      = name.find("|");
    std::string attribute = "";

    if (pipe != std::string::npos)
    {
        attribute = name.substr(pipe + 1);
        name      = name.substr(0, pipe);
    }


    /*
     * The resulting colour-pair.
     */
    int result = COLOR_PAIR(m_colours[name]);

    /*
     * Now handle the attribute too.
     */
    if (attribute.empty())
    {
        result |= A_NORMAL;
    }
    else
    {
        if (attribute.find("underline") != std::string::npos)
            result |= A_UNDERLINE;

        if (attribute.find("standout") != std::string::npos)
            result |= A_STANDOUT;

        if (attribute.find("reverse") != std::string::npos)
            result |= A_REVERSE;

        if (attribute.find("blink") != std::string::npos)
            result |= A_BLINK;

        if (attribute.find("dim") != std::string::npos)
            result |= A_DIM;

        if (attribute.find("bold") != std::string::npos)
            result |= A_BOLD;

        if (attribute.find("normal") != std::string::npos)
        {
            result &= A_UNDERLINE;
            result &= A_STANDOUT;
            result &= A_REVERSE;
            result &= A_BLINK;
            result &= A_DIM;
            result &= A_BOLD;
            result |= A_NORMAL;
        }
    }

    return (result);
}



/*
 * Draw an array of lines to the screen, highlighting the current line.
 *
 * This is used by our view-classes, as a helper.
 *
 * If `simple` is set to true then we display the lines in a  simplified
 * fashion - with no selection, and no smooth-scrolling.
 *
 */
void CScreen::draw_text_lines(std::vector<std::string> lines, int selected, int max, bool simple)
{
    /*
     * Get the dimensions of the screen.
     */
    CScreen *screen = CScreen::instance();
    int height      = CScreen::height();
    int width       = CScreen::width();

    /*
     * Is line-wrapping enabled?
     */
    CConfig *config = CConfig::instance();
    int wrap = config->get_integer("line.wrap", 0);

    /*
     * Take off the panel, if visible.
     */
    CStatusPanel *panel = CStatusPanel::instance();

    if (panel->hidden() == false)
        height -= panel->height();

    /*
     * Add an extra line.
     */
    height += 1;

    /*
     * If we're in simple-mode we can just draw the lines directly
     * and return - we don't need to worry about the selection-handler
     * or the calculation of the scroll-point.
     */
    if (simple)
    {
        /*
         * The number of lines to draw.
         */
        int size = lines.size();

        /*
         * The width of each line drawn.
         */
        int result __attribute__((unused));

        int off = 0;

        for (int i = 0; i <= height; i++)
        {
            std::string buf = "";

            /*
             * If we're still in the array of lines to draw
             * then pick the right one.
             */
            if ((off + selected) < size)
                buf = lines.at(off + selected);

            /*
             * Last two parameters are:
             *
             *  enable scroll: true
             *  enable wrap: true
             */
            result = draw_single_line(i, 0, buf, stdscr, true, true);

            /*
             * Did we draw more than a single line?
             */
            if (result > width)
            {
                /*
                 * If we've got wrapping enabled bump to the next
                 * line.
                 */
                if (wrap == 1)
                    i += (result / width);
            }

            off += 1;
        }

        return;
    }


    /*
     * This is complex/smooth-scrolling mode.
     *
     * We'll draw a highlighted bar, and that'll move "nicely".
     */
    int middle = (height) / 2;
    int rowToHighlight = 0;
    vectorPosition topBottomOrMiddle = NONE;

    /*
     * default to TOP if our list is shorter then the screen height
     */
    if (selected < middle || max <= height)
    {
        topBottomOrMiddle = TOP;
        rowToHighlight = selected;

        /*
         * if height is uneven we have to switch to the BOTTOM case on row earlier
         */
    }
    else if ((max - selected <= middle) || (height % 2 == 1 && max - selected <= middle + 1))
    {
        topBottomOrMiddle = BOTTOM;
        rowToHighlight =  height - max + selected - 1 ;
    }
    else
    {
        topBottomOrMiddle = MIDDLE;
        rowToHighlight = middle;
    }


    for (int row = 0; row <= height; row++)
    {
        /*
         * The current object.
         */
        int mailIndex = max;
        int size      = lines.size();


        if (topBottomOrMiddle == TOP)
        {
            /*
             * we start at the top of the list so just use row
             */
            mailIndex = row;
        }
        else if (topBottomOrMiddle == BOTTOM)
        {
            /*
             * when we reached the end of the list mailIndex can maximally be
             * count-1, that this is given can easily be shown
             * row:=height-2 -> count-height+row+1 = count-height+height-2+1 = count-1
             */
            mailIndex = max - height + row + 1;
        }
        else if (topBottomOrMiddle == MIDDLE)
        {
            mailIndex = row + selected - middle;
        }


        std::string buf;

        if ((mailIndex < max) && (mailIndex < size))
            buf = lines.at(mailIndex);

        if (buf.empty())
            continue;

        if (row == rowToHighlight)
            wattrset(stdscr, A_REVERSE | A_STANDOUT);
        else
            wattrset(stdscr, A_NORMAL);

        /*
         * Last two parameters are:
         *
         *  enable scroll: true
         *  enable wrap: false
         */
        int result __attribute__((unused));

        /*
         * Last two parameters are:
         *
         *  enable scroll: true
         *  enable wrap: false
         */
        result = draw_single_line(row, 0, buf, stdscr, true, false);
    }

    /*
     * Ensure we turn off the attribute on the last line - so that
     * any blank lines are "normal".
     */
    wattrset(stdscr, screen->get_colour("white|normal"));
}



/*
 * Draw a single text line, paying attention to our colour strings.
 *
 * This needs to handle two special-cases:
 *
 *  * The formatting of coloured-input.
 *
 *  * The handling of horizontal scrolling via `global.horizontal`.
 *
 * The return value is the number of characters drawn.
 */
int CScreen::draw_single_line(int row, int col_offset, std::string buf, WINDOW * screen, bool enable_scroll, bool enable_wrap)
{
    /*
     * Move the cursor to the correct location.
     */
    wmove(screen, row, col_offset);

    /*
     * Later we'll get the the position of the cursor to detect wrapping.
     */
    int x, y;

    /*
     * Count of characters we drew.
     */
    int count = 0;

    /*
     * Default colour/attributes for this line.
     */
    int def_col = getattrs(stdscr);

    /*
     * Get the horizontal scroll offset.
     */
    CConfig *config = CConfig::instance();
    int horiz = config->get_integer("global.horizontal", 0);

    /*
     * Is wrapping enabled?
     *
     * Here we only allow this to be enabled if this function
     * was called with `enable_wrap: true`.  This is so that
     * we don't try to pointlessly enable wrap for modes that
     * it doesn't make sense with.
     */
    int wrap = config->get_integer("line.wrap", 0);

    if ((wrap != 0) && (enable_wrap == true))
        enable_wrap = true;

    /*
     * If scrolling is disabled (i.e. drawing the panel) we
     * reset the horiz-position.
     */
    if (enable_scroll == false)
        horiz = 0;

    /*
     * Split the string into segments in ONE CHARACTER pieces.
     *
     * These single characters may well consist of multiple bytes, but
     * that is not important.
     */
    std::vector<COLOUR_STRING *> parts = CColourString::parse_coloured_string(buf, horiz);

    /*
     * Draw each piece of the string.
     */
    for (auto it = parts.begin(); it != parts.end() ; ++it)
    {
        /*
         * If we've drawn more characters than the width
         * of the screen then we should stop - unless we've got
         * wrapping enabled.
         */
        getyx(screen, y, x);

        if ((y != row) && ! enable_wrap)
            continue;

        /*
         * Get the text/colour.
         */
        COLOUR_STRING *i = (*it);
        std::string *colour = i->colour;
        std::string *text   = i->string;

        /*
         * Set the colour + draw the component.
         */
        wattrset(screen, def_col);
        wattron(screen, get_colour(*colour));
        waddstr(screen, (char *)(*text).c_str());

        count += 1;
    }


    /*
     * We now pad the text we've drawn to ensure that it fills a complete
     * line.  We need to do this to ensure that any highlighting, underlining,
     * or blinking persists to the end of the line.
     */

    getyx(screen, y, x);

    while (y == row)
    {
        /*
         * Draw " " over and over again, until we wrap and the row
         * changes to the next one.
         */
        waddstr(screen, (char *)" ");
        getyx(screen, y, x);

        count += 1;
    }


    /*
     * Reset to our default colour.
     */
    wattrset(screen, get_colour("white|normal"));

    /*
     * Finally free the tokenized string-bits.
     */
    for (auto it = parts.begin(); it != parts.end() ; ++it)
    {
        COLOUR_STRING *i = (*it);
        delete(i->string);
        delete(i->colour);
        free(i);
    }

    return (count);
}

/*
 * Draw a single piece of text, allowing colours too.
 */
void CScreen::draw_text(int x, int y, std::string str)
{
    /*
     * Default colour/attributes for this line.
     */
    int def_col = getattrs(stdscr);

    /*
     * Parse the string.  We don't need it in chunks,
     * but we do need it in coloured fashion.
     */
    std::vector<COLOUR_STRING *> parts;
    parts = CColourString::parse_coloured_string(str, 0);

    /*
     * Move to the starting offset.
     */
    wmove(stdscr, y, x);

    /*
     * Draw the part(s).
     */
    for (auto it = parts.begin(); it != parts.end() ; ++it)
    {
        /*
         * Get the text/colour.
         */
        COLOUR_STRING *i = (*it);
        std::string *colour = i->colour;
        std::string *text   = i->string;

        /*
         * Set the colour + draw the component.
         */
        wattrset(stdscr, def_col);
        wattron(stdscr, get_colour(*colour));
        waddstr(stdscr, (char *)(*text).c_str());
    }

    /*
     * Reset to our default colour.
     */
    wattrset(stdscr, get_colour("white|normal"));

    /*
     * Free the parts.
     */
    for (auto it = parts.begin(); it != parts.end() ; ++it)
    {
        COLOUR_STRING *i = (*it);
        delete(i->string);
        delete(i->colour);
        free(i);
    }


}
