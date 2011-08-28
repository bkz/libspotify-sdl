#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include <queue>

#include "getopt.h"

/////////////////////////////////////////////////////////////////////////////
// Third-party libraries.
/////////////////////////////////////////////////////////////////////////////

#include "libspotify/api.h"

#ifdef _MSC_VER
    #pragma comment(lib, "libspotify.lib")
#endif

#include "SDL/SDL.h"
#include "SDL/SDL_audio.h"
#include "SDL/SDL_thread.h"

#ifdef __APPLE__
    #undef main // No SDLmain.lib on OSX.
#endif

#ifdef _MSC_VER
    #define strcasecmp _stricmp
    #pragma comment(lib, "SDL.lib")
    #pragma comment(lib, "SDLmain.lib")
#endif

/////////////////////////////////////////////////////////////////////////////
// Libspotify application key.
/////////////////////////////////////////////////////////////////////////////

const uint8_t g_appkey[] = {
    //
    // PASTE YOUR SPOTIFY APPLICATION KEY HERE!
    //
};

/////////////////////////////////////////////////////////////////////////////
// Global variables.
/////////////////////////////////////////////////////////////////////////////

/// Synchronization mutex for the main thread
static SDL_mutex* g_notify_mutex;
/// Synchronization condition variable for the main thread
static SDL_cond* g_notify_cond;
/// Synchronization variable telling the main thread to process events
static int g_notify_do;
/// Non-zero when a track has ended and the jukebox has not yet started a new one
static int g_playback_done;
/// The global session handle
static sp_session *g_sess;
/// Handle to the playlist currently being played
static sp_playlist *g_jukeboxlist;
/// Name of the playlist currently being played
const char *g_listname;
/// Remove tracks flag
static int g_remove_tracks = 0;
/// Handle to the curren track
static sp_track *g_currenttrack;
/// Index to the next track
static int g_track_index;

/////////////////////////////////////////////////////////////////////////////
// SDL audio bridge.
/////////////////////////////////////////////////////////////////////////////

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;

enum {
    FADE_IN_AUDIO_DURATION_SECS = 10,
};

enum {
    SDL_AUDIO_SAMPLE_SIZE = sizeof(uint16_t),
    SDL_AUDIO_SAMPLE_RATE = 44100,
    SDL_AUDIO_CHANNELS = 2,
    SDL_AUDIO_BUFFER_FRAMES = 2048,
};

enum {
    SDL_AUDIO_REQUEST_SIZE = (SDL_AUDIO_BUFFER_FRAMES * SDL_AUDIO_SAMPLE_SIZE * SDL_AUDIO_CHANNELS),
    MAX_REQUEST_BUFFER_COUNT = (1 * SDL_AUDIO_SAMPLE_RATE) / SDL_AUDIO_BUFFER_FRAMES,
};

/// Shared audio queue for pushing data between libspotify and SDL
static std::queue<uint8_t*> g_audio_queue;

/// Synchronization mutex for SDL audio buffering
static SDL_mutex* g_audio_mutex;

static int audio_delivery(sp_session *sess, const sp_audioformat *format,
                          const void *frames, int num_frames)
{
    SDL_mutexP(g_audio_mutex);

    if (num_frames == 0)
    {
        SDL_mutexV(g_audio_mutex);
        return 0;
    }

    if (g_audio_queue.size() > MAX_REQUEST_BUFFER_COUNT)
    {
        SDL_mutexV(g_audio_mutex);
        return 0;
    }

    assert(format->sample_rate == SDL_AUDIO_SAMPLE_RATE);
    assert(format->channels == SDL_AUDIO_CHANNELS);
    assert(format->sample_type == SP_SAMPLETYPE_INT16_NATIVE_ENDIAN);

    //
    // Push SDL_AUDIO_BUFFER_FRAMES to the audio layer even if `num_frames`
    // delievered by Spotify is less than this (worst case is 1 second silence
    // if num_frames = 1 since we feed SDL chunks of 1 second audio at a time).
    //
    // NOTE: Abusing the memory allocator like this should be pretty OK since
    // we're basically re-using the same chunks all over again. Yeah I'm lazy.'
    //

    {
        uint8_t* chunk = new uint8_t[SDL_AUDIO_REQUEST_SIZE];
        memset(chunk, 0, SDL_AUDIO_REQUEST_SIZE);
        memcpy(chunk, frames, num_frames * SDL_AUDIO_SAMPLE_SIZE * SDL_AUDIO_CHANNELS);
        g_audio_queue.push(chunk);
    }

    SDL_mutexV(g_audio_mutex);

    return SDL_AUDIO_BUFFER_FRAMES;
}

static void audio_mix(void* unused, Uint8* stream, int len)
{
    assert(len == SDL_AUDIO_REQUEST_SIZE);

    double elapsed = double(clock()) / double(CLOCKS_PER_SEC);

    int volume = SDL_MIX_MAXVOLUME;

    if (elapsed < FADE_IN_AUDIO_DURATION_SECS)
        volume = int(double(volume) * double(elapsed) / double(FADE_IN_AUDIO_DURATION_SECS));

    if (!g_playback_done && g_audio_queue.size() > 0)
    {
        SDL_mutexP(g_audio_mutex);
        uint8_t* chunk = g_audio_queue.front();
        SDL_MixAudio(stream, chunk, SDL_AUDIO_REQUEST_SIZE, volume);
        delete[] chunk;
        g_audio_queue.pop();
        SDL_mutexV(g_audio_mutex);
    }
}

static void audio_flush()
{
    SDL_mutexP(g_audio_mutex);

    while (g_audio_queue.size() > 0)
    {
        uint8_t* chunk = g_audio_queue.front();
        delete[] chunk;
        g_audio_queue.pop();
    }

    SDL_mutexV(g_audio_mutex);
}

static bool audio_open()
{
    g_audio_mutex = SDL_CreateMutex();

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
    {
        return false;
    }

    SDL_AudioSpec as;
    as.freq = SDL_AUDIO_SAMPLE_RATE;
    as.format = AUDIO_S16SYS; // Match SP_SAMPLETYPE_INT16_NATIVE_ENDIAN.
    as.channels = SDL_AUDIO_CHANNELS;
    as.samples = SDL_AUDIO_BUFFER_FRAMES; // LRLRLRLR...
    as.callback = audio_mix;

    SDL_AudioSpec obtained;
    if (SDL_OpenAudio(&as, &obtained) < 0)
    {
        return false;
    }

    if (obtained.format != AUDIO_S16SYS)
    {
        return false;
    }

    SDL_PauseAudio(false);

    return true;
}


void audio_close(void)
{
    SDL_DestroyMutex(g_audio_mutex);
    SDL_PauseAudio(true);
    SDL_CloseAudio();
}

/////////////////////////////////////////////////////////////////////////////
// Libspotify demo (straight port of jukebox.c from the SDK).
/////////////////////////////////////////////////////////////////////////////

/**
 * Called on various events to start playback if it hasn't been started already.
 *
 * The function simply starts playing the first track of the playlist.
 */
static void try_jukebox_start(void)
{
    sp_track *t;

    if (!g_jukeboxlist)
        return;

    if (!sp_playlist_num_tracks(g_jukeboxlist)) {
        fprintf(stderr, "jukebox: No tracks in playlist. Waiting\n");
        return;
    }

    if (sp_playlist_num_tracks(g_jukeboxlist) < g_track_index) {
        fprintf(stderr, "jukebox: No more tracks in playlist. Waiting\n");
        return;
    }

    t = sp_playlist_track(g_jukeboxlist, g_track_index);

    if (g_currenttrack && t != g_currenttrack) {
        /* Someone changed the current track */
        audio_flush();
        sp_session_player_unload(g_sess);
        g_currenttrack = NULL;
    }

    if (!t)
        return;

    if (sp_track_error(t) != SP_ERROR_OK)
        return;

    if (g_currenttrack == t)
        return;

    g_currenttrack = t;

    printf("jukebox: Now playing \"%s\"...\n", sp_track_name(t));
    fflush(stdout);

    sp_session_player_load(g_sess, t);
    sp_session_player_play(g_sess, 1);
}

/* --------------------------  PLAYLIST CALLBACKS  ------------------------- */
/**
 * Callback from libspotify, saying that a track has been added to a playlist.
 *
 * @param  pl          The playlist handle
 * @param  tracks      An array of track handles
 * @param  num_tracks  The number of tracks in the \c tracks array
 * @param  position    Where the tracks were inserted
 * @param  userdata    The opaque pointer
 */
static void SP_CALLCONV tracks_added(sp_playlist *pl, sp_track * const *tracks,
                         int num_tracks, int position, void *userdata)
{
    if (pl != g_jukeboxlist)
        return;

    printf("jukebox: %d tracks were added\n", num_tracks);
    fflush(stdout);
    try_jukebox_start();
}

/**
 * Callback from libspotify, saying that a track has been added to a playlist.
 *
 * @param  pl          The playlist handle
 * @param  tracks      An array of track indices
 * @param  num_tracks  The number of tracks in the \c tracks array
 * @param  userdata    The opaque pointer
 */
static void SP_CALLCONV tracks_removed(sp_playlist *pl, const int *tracks,
                           int num_tracks, void *userdata)
{
    int i, k = 0;

    if (pl != g_jukeboxlist)
        return;

    for (i = 0; i < num_tracks; ++i)
        if (tracks[i] < g_track_index)
            ++k;

    g_track_index -= k;

    printf("jukebox: %d tracks were removed\n", num_tracks);
    fflush(stdout);
    try_jukebox_start();
}

/**
 * Callback from libspotify, telling when tracks have been moved around in a playlist.
 *
 * @param  pl            The playlist handle
 * @param  tracks        An array of track indices
 * @param  num_tracks    The number of tracks in the \c tracks array
 * @param  new_position  To where the tracks were moved
 * @param  userdata      The opaque pointer
 */
static void SP_CALLCONV tracks_moved(sp_playlist *pl, const int *tracks,
                         int num_tracks, int new_position, void *userdata)
{
    if (pl != g_jukeboxlist)
        return;

    printf("jukebox: %d tracks were moved around\n", num_tracks);
    fflush(stdout);
    try_jukebox_start();
}

/**
 * Callback from libspotify. Something renamed the playlist.
 *
 * @param  pl            The playlist handle
 * @param  userdata      The opaque pointer
 */
static void SP_CALLCONV playlist_renamed(sp_playlist *pl, void *userdata)
{
    const char *name = sp_playlist_name(pl);

    if (!strcasecmp(name, g_listname)) {
        g_jukeboxlist = pl;
        g_track_index = 0;
        try_jukebox_start();
    } else if (g_jukeboxlist == pl) {
        printf("jukebox: current playlist renamed to \"%s\".\n", name);
        g_jukeboxlist = NULL;
        g_currenttrack = NULL;
        sp_session_player_unload(g_sess);
    }
}

/**
 * The callbacks we are interested in for individual playlists.
 */
static sp_playlist_callbacks pl_callbacks;

static void init_playlist_callbacks()
{
    pl_callbacks.tracks_added = tracks_added;
    pl_callbacks.tracks_removed = tracks_removed;
    pl_callbacks.tracks_moved = tracks_moved;
    pl_callbacks.playlist_renamed = playlist_renamed;
};


/* --------------------  PLAYLIST CONTAINER CALLBACKS  --------------------- */
/**
 * Callback from libspotify, telling us a playlist was added to the playlist container.
 *
 * We add our playlist callbacks to the newly added playlist.
 *
 * @param  pc            The playlist container handle
 * @param  pl            The playlist handle
 * @param  position      Index of the added playlist
 * @param  userdata      The opaque pointer
 */
static void SP_CALLCONV playlist_added(sp_playlistcontainer *pc, sp_playlist *pl,
                           int position, void *userdata)
{
    sp_playlist_add_callbacks(pl, &pl_callbacks, NULL);

    if (!strcasecmp(sp_playlist_name(pl), g_listname)) {
        g_jukeboxlist = pl;
        try_jukebox_start();
    }
}

/**
 * Callback from libspotify, telling us a playlist was removed from the playlist container.
 *
 * This is the place to remove our playlist callbacks.
 *
 * @param  pc            The playlist container handle
 * @param  pl            The playlist handle
 * @param  position      Index of the removed playlist
 * @param  userdata      The opaque pointer
 */
static void SP_CALLCONV playlist_removed(sp_playlistcontainer *pc, sp_playlist *pl,
                             int position, void *userdata)
{
    sp_playlist_remove_callbacks(pl, &pl_callbacks, NULL);
}


/**
 * Callback from libspotify, telling us the rootlist is fully synchronized
 * We just print an informational message
 *
 * @param  pc            The playlist container handle
 * @param  userdata      The opaque pointer
 */
static void SP_CALLCONV container_loaded(sp_playlistcontainer *pc, void *userdata)
{
    fprintf(stderr, "jukebox: Rootlist synchronized (%d playlists)\n",
        sp_playlistcontainer_num_playlists(pc));
}


/**
 * The playlist container callbacks
 */
static sp_playlistcontainer_callbacks pc_callbacks;
static void init_playlist_container_callbacks()
{
    pc_callbacks.playlist_added = playlist_added;
    pc_callbacks.playlist_removed = playlist_removed;
    pc_callbacks.container_loaded = container_loaded;
};


/* ---------------------------  SESSION CALLBACKS  ------------------------- */
/**
 * This callback is called when an attempt to login has succeeded or failed.
 *
 * @sa sp_session_callbacks#logged_in
 */
static void SP_CALLCONV logged_in(sp_session *sess, sp_error error)
{
    sp_playlistcontainer *pc = sp_session_playlistcontainer(sess);
    int i;

    if (SP_ERROR_OK != error) {
        fprintf(stderr, "jukebox: Login failed: %s\n",
            sp_error_message(error));
        exit(2);
    }

    printf("jukebox: Looking at %d playlists\n", sp_playlistcontainer_num_playlists(pc));

    for (i = 0; i < sp_playlistcontainer_num_playlists(pc); ++i) {
        sp_playlist *pl = sp_playlistcontainer_playlist(pc, i);

        sp_playlist_add_callbacks(pl, &pl_callbacks, NULL);

        if (!strcasecmp(sp_playlist_name(pl), g_listname)) {
            g_jukeboxlist = pl;
            try_jukebox_start();
        }
    }

    if (!g_jukeboxlist) {
        printf("jukebox: No such playlist. Waiting for one to pop up...\n");
        fflush(stdout);
    }
}

/**
 * This callback is called from an internal libspotify thread to ask us to
 * reiterate the main loop.
 *
 * We notify the main thread using a condition variable and a protected variable.
 *
 * @sa sp_session_callbacks#notify_main_thread
 */
static void SP_CALLCONV notify_main_thread(sp_session *sess)
{
    SDL_mutexP(g_notify_mutex);
    g_notify_do = 1;
    SDL_CondSignal(g_notify_cond);
    SDL_mutexV(g_notify_mutex);
}

/**
 * This callback is used from libspotify whenever there is PCM data available.
 *
 * @sa sp_session_callbacks#music_delivery
 */
static int SP_CALLCONV music_delivery(sp_session *sess, const sp_audioformat *format,
                          const void *frames, int num_frames)
{
    return audio_delivery(sess, format, frames, num_frames);
}


/**
 * This callback is used from libspotify when the current track has ended
 *
 * @sa sp_session_callbacks#end_of_track
 */
static void SP_CALLCONV end_of_track(sp_session *sess)
{
    SDL_mutexP(g_notify_mutex);
    g_playback_done = 1;
    SDL_CondSignal(g_notify_cond);
    SDL_mutexV(g_notify_mutex);
}


/**
 * Callback called when libspotify has new metadata available
 *
 * Not used in this example (but available to be able to reuse the session.c file
 * for other examples.)
 *
 * @sa sp_session_callbacks#metadata_updated
 */
static void SP_CALLCONV metadata_updated(sp_session *sess)
{
    try_jukebox_start();
}

/**
 * Notification that some other connection has started playing on this account.
 * Playback has been stopped.
 *
 * @sa sp_session_callbacks#play_token_lost
 */
static void SP_CALLCONV play_token_lost(sp_session *sess)
{
    audio_flush();

    if (g_currenttrack != NULL) {
        sp_session_player_unload(g_sess);
        g_currenttrack = NULL;
    }
}

/**
 * The session callbacks
 */
static sp_session_callbacks session_callbacks;
static void init_session_callbacks()
{
    session_callbacks.logged_in = logged_in;
    session_callbacks.notify_main_thread = notify_main_thread;
    session_callbacks.music_delivery = music_delivery;
    session_callbacks.metadata_updated = metadata_updated;
    session_callbacks.play_token_lost = play_token_lost;
    session_callbacks.log_message = NULL;
    session_callbacks.end_of_track = end_of_track;
};

/**
 * The session configuration. Note that application_key_size is an external, so
 * we set it in main() instead.
 */
static sp_session_config spconfig;
static void init_session_config()
{
    spconfig.api_version = SPOTIFY_API_VERSION;
    spconfig.cache_location = "temp";
    spconfig.settings_location = "temp";
    spconfig.application_key = g_appkey;
    spconfig.application_key_size = sizeof(g_appkey);
    spconfig.user_agent = "spotify-jukebox-example";
    spconfig.callbacks = &session_callbacks;
};
/* -------------------------  END SESSION CALLBACKS  ----------------------- */


/**
 * A track has ended. Remove it from the playlist.
 *
 * Called from the main loop when the music_delivery() callback has set g_playback_done.
 */
static void track_ended(void)
{
    int tracks = 0;

    if (g_currenttrack) {
        g_currenttrack = NULL;
        sp_session_player_unload(g_sess);
        if (g_remove_tracks) {
            sp_playlist_remove_tracks(g_jukeboxlist, &tracks, 1);
        } else {
            ++g_track_index;
            try_jukebox_start();
        }
    }
}

/**
 * Show usage information
 *
 * @param  progname  The program name
 */
static void usage(const char *progname)
{
    fprintf(stderr, "usage: %s -u <username> -p <password> -l <listname> [-d]\n", progname);
    fprintf(stderr, "warning: -d will delete the tracks played from the list!\n");
}

int main(int argc, char **argv)
{
    init_session_config();
    init_session_callbacks();
    init_playlist_callbacks();
    init_playlist_container_callbacks();

    sp_session *sp;
    sp_error err;
    int next_timeout = 0;
    const char *username = NULL;
    const char *password = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "u:p:l:d")) != EOF) {
        switch (opt) {
        case 'u':
            username = optarg;
            break;

        case 'p':
            password = optarg;
            break;

        case 'l':
            g_listname = optarg;
            break;

        case 'd':
            g_remove_tracks = 1;
            break;

        default:
            exit(1);
        }
    }

    if (!username || !password || !g_listname) {
        usage(argv[0]);
        exit(1);
    }

    audio_open();

    err = sp_session_create(&spconfig, &sp);

    if (SP_ERROR_OK != err) {
        fprintf(stderr, "Unable to create session: %s\n",
            sp_error_message(err));
        exit(1);
    }

    g_sess = sp;

    g_notify_mutex = SDL_CreateMutex();
    g_notify_cond = SDL_CreateCond();


    sp_playlistcontainer_add_callbacks(
        sp_session_playlistcontainer(g_sess),
        &pc_callbacks,
        NULL);

    sp_session_login(sp, username, password);
    SDL_mutexP(g_notify_mutex);

    for (;;) {
        if (next_timeout == 0) {
            while(!g_notify_do && !g_playback_done)
                SDL_CondWait(g_notify_cond, g_notify_mutex);
        } else {
            SDL_CondWaitTimeout(g_notify_cond, g_notify_mutex, next_timeout);
        }

        g_notify_do = 0;
        SDL_mutexV(g_notify_mutex);

        if (g_playback_done) {
            track_ended();
            g_playback_done = 0;
        }

        do {
            sp_session_process_events(sp, &next_timeout);
        } while (next_timeout == 0);

        SDL_mutexP(g_notify_mutex);
    }

    return 0;
}

/////////////////////////////////////////////////////////////////////////////
// The End.
/////////////////////////////////////////////////////////////////////////////
