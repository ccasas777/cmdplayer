// CamillaDSP ALSA "IO" Plugin (Maybe O plugin as it is output only)
//
// Based on bluealsa-pcm.c from the project bluez-alsa
// which is Copyright (c) 2016-2020 Arkadiusz Bokowy
//
// This project is licensed under the terms of the MIT license.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <limits.h>

#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "strrep.h"

#define DEBUG 1
#define error(fmt, ...) \
  do { if(DEBUG > 0){fprintf(stderr,"CDSP Plugin ERROR: ");\
    fprintf(stderr,((fmt)), ##__VA_ARGS__);} } while (0)
#define warn(fmt, ...) \
  do { if(DEBUG > 1){fprintf(stderr,"CDSP Plugin WARN: ");\
    fprintf(stderr,((fmt)), ##__VA_ARGS__);} } while (0)
#define info(fmt, ...) \
  do { if(DEBUG > 2){fprintf(stderr,"CDSP Plugin INFO: ");\
    fprintf(stderr,((fmt)), ##__VA_ARGS__);} } while (0)
#define debug(fmt, ...) \
  do { if(DEBUG > 3){fprintf(stderr,"CDSP Plugin DEBUG: ");\
    fprintf(stderr,((fmt)), ##__VA_ARGS__);} } while (0)
#define excessive(fmt, ...) \
  do { if(DEBUG > 4){fprintf(stderr,((fmt)), ##__VA_ARGS__);} } while (0)

#define CDSP_PAUSE_STATE_RUNNING 0
#define CDSP_PAUSE_STATE_PAUSED  (1 << 0)
#define CDSP_PAUSE_STATE_PENDING (1 << 1)

// Cleanup callback casting wrapper for the brevity's sake.
#define PTHREAD_CLEANUP(f) ((void (*)(void *))(void (*)(void))(f))

// Thread routing callback casting wrapper.
#define PTHREAD_ROUTINE(f) ((void *(*)(void *))(f))

//
// Get system monotonic time-stamp.
//
// Why try for RAW?  We're trying to simulate an accurate clock.  Let
// ntp correct the rate.
//
// @param ts Address to the timespec structure where the time-stamp will
// be stored.
// @return On success this function returns 0. Otherwise, -1 is returned
// and errno is set to indicate the error.
//#ifdef CLOCK_MONOTONIC_RAW
//# define gettimestamp(ts) clock_gettime(CLOCK_MONOTONIC_RAW, ts)
//#else
# define gettimestamp(ts) clock_gettime(CLOCK_MONOTONIC, ts)
//#endif

// Calculate time difference for two time points.
// 
//  @param ts1 Address to the timespec structure providing t1 time point.
//  @param ts2 Address to the timespec structure providing t2 time point.
//  @param ts Address to the timespec structure where the absolute time
//    difference will be stored.
// @return This function returns an integer less than, equal to, or greater
//    than zero, if t2 time point is found to be, respectively, less than,
//    equal to, or greater than the t1 time point.*/
int difftimespec( const struct timespec *ts1, const struct timespec *ts2,
    struct timespec *ts) {
  const struct timespec _ts1 = *ts1;
  const struct timespec _ts2 = *ts2;

  if (_ts1.tv_sec == _ts2.tv_sec) {
    ts->tv_sec = 0;
    ts->tv_nsec = labs(_ts2.tv_nsec - _ts1.tv_nsec);
    return _ts2.tv_nsec > _ts1.tv_nsec ? 1 : -ts->tv_nsec;
  }

  if (_ts1.tv_sec < _ts2.tv_sec) {
    if (_ts1.tv_nsec <= _ts2.tv_nsec) {
      ts->tv_sec = _ts2.tv_sec - _ts1.tv_sec;
      ts->tv_nsec = _ts2.tv_nsec - _ts1.tv_nsec;
    }
    else {
      ts->tv_sec = _ts2.tv_sec - 1 - _ts1.tv_sec;
      ts->tv_nsec = _ts2.tv_nsec + 1000000000 - _ts1.tv_nsec;
    }
    return 1;
  }

  if (_ts1.tv_nsec >= _ts2.tv_nsec) {
    ts->tv_sec = _ts1.tv_sec - _ts2.tv_sec;
    ts->tv_nsec = _ts1.tv_nsec - _ts2.tv_nsec;
  }
  else {
    ts->tv_sec = _ts1.tv_sec - 1 - _ts2.tv_sec;
    ts->tv_nsec = _ts1.tv_nsec + 1000000000 - _ts2.tv_nsec;
  }
  return -1;
}

typedef struct {
  snd_pcm_ioplug_t io;

  // IO thread and application thread sync
  pthread_mutex_t mutex;

  // Pipe to talk to CamillaDSP
  int cdsp_pcm_fd;

  // event file descriptor
  int event_fd;

  // virtual hardware - ring buffer
  char *io_hw_buffer;

  // The IO thread is responsible for maintaining the hardware pointer
  // (pcm->io_hw_ptr), the application is responsible for the application
  // pointer (io->appl_ptr). These are both volatile as they are both
  // written in one thread and read in the other.
  volatile snd_pcm_uframes_t io_hw_ptr;
  // A signed value for the status return to the IO plugin pointer call
  volatile int io_status;
  snd_pcm_uframes_t io_hw_boundary;
  // Permit the application to modify the frequency of poll() events.
  volatile snd_pcm_uframes_t io_avail_min;
  pthread_t io_thread;
  bool io_started;
  // io_thread was created and not yet joined.  It stays set after the
  // thread ends by itself, while io_started is cleared then.
  bool io_joinable;

  // ALSA operates on frames, we on bytes
  size_t frame_size;

  struct timespec delay_ts;
  snd_pcm_uframes_t delay_hw_ptr;
  unsigned int delay_pcm_nread;
  // In the capture mode, delay_running indicates that frames are being
  // transfered to the FIFO by the server. In playback mode it indicates
  // that the IO thread is transferring frames to the FIFO.
  bool delay_running;

  // delay accumulated just before pausing
  snd_pcm_sframes_t delay_paused;
  // maximum delay in FIFO
  snd_pcm_sframes_t delay_fifo_size;

  // synchronize threads to begin/end pause
  pthread_cond_t pause_cond;
  unsigned int pause_state;

  // Process id of forked CamillaDSP
  pid_t cpid;
  // Path to CamillaDSP executable
  char *cpath;
  // Location of CamillaDSP input YAML template
  char *config_in;
  // Alternatively to providing config_in provide a program
  // that will generate config_out
  char *config_cmd;
  // And yet another alternative use CamillaDSP's new internal
  // subsitution
  long config_cdsp;
  // Gain and mute startup file
  char *vol_file;
  // Arguments to execv
  // cargs[0] = "camilladsp" => Process name
  // cargs[1] = config_out => Location of CamillaDSP output YAML configuration
  // cargs[2+] = Additional arguments passed through .asoundrc
  // If config_cdsp cargs will also be used to hold hw_params
  // If vol_file is set cargs will also be used to hold mute and gain
  // Make the array a bit bigger to allow them
  size_t n_cargs;
  char *cargs[120];
  // Search / Replace string tokens - let people use whatever format
  // they want.
  char *format_token;
  char *rate_token;
  char *channels_token;
  char *ext_samp_token;
  // Extra samples parameter to pass to CamillaDSP if the config_in template
  // is used instead of config_cmd
  // ext_samp_44100 and ext_samp_4800 allow rate matched expansion of the
  // extra samples.  They will be multiplied by {rate}/44100 or rate/{48000}
  // if {rate} is an integer multiple of 44100 or 48000 respectively
  long ext_samp;
  long ext_samp_44100;
  long ext_samp_48000;

  // Suppress a spurious warning on the first call to revents for the
  // event triggered during prepare.  Some programs need that event to
  // start so it's not actually an overcall.
  bool first_revent;
  // A command to run at init - turn on audio system for example
  char *start_cmd;
  // A command to run just before the CamillaDSP process is ended
  // A good time to update the gain and mute values for example
  char *camilla_exit_cmd;
  // Requested size in bytes of the pipe to CamillaDSP
  long fifo_size;
  // How long to wait in ms for CamillaDSP to exit after closing the pipe
  // before it is terminated
  long stop_timeout;
  // Terminate CamillaDSP at once when the stream ends without a drain,
  // instead of letting it play what is still queued
  bool kill_on_drop;
  // The last stream was drained to the end
  bool drained;
} cdsp_t;

#define CDSP_N_FORMATS 6

// CamillaDSP (v4 and later) sample format name for an ALSA format, or NULL
// if CamillaDSP has no matching format.
static const char *cdsp_format_name(snd_pcm_format_t format) {
  switch (format) {
    case SND_PCM_FORMAT_S16_LE:
      return "S16_LE";
    case SND_PCM_FORMAT_S24_LE:
      return "S24_4_RJ_LE";
    case SND_PCM_FORMAT_S24_3LE:
      return "S24_3_LE";
    case SND_PCM_FORMAT_S32_LE:
      return "S32_LE";
    case SND_PCM_FORMAT_FLOAT_LE:
      return "F32_LE";
    case SND_PCM_FORMAT_FLOAT64_LE:
      return "F64_LE";
    default:
      return NULL;
  }
}

#if SND_LIB_VERSION < 0x010106
//
// Get the available frames.
//
// This function is available in alsa-lib since version 1.1.6. For older
// alsa-lib versions we need to provide our own implementation.
static snd_pcm_uframes_t snd_pcm_ioplug_hw_avail(
    const snd_pcm_ioplug_t * const io, const snd_pcm_uframes_t hw_ptr, 
    const snd_pcm_uframes_t appl_ptr) {
  cdsp_t *pcm = io->private_data;
  snd_pcm_sframes_t diff;
  if (io->stream == SND_PCM_STREAM_PLAYBACK)
    diff = appl_ptr - hw_ptr;
  else
    diff = io->buffer_size - hw_ptr + appl_ptr;
  if (diff < 0)
    diff += pcm->io_hw_boundary;
  return diff <= io->buffer_size ? (snd_pcm_uframes_t) diff : 0;
}
#endif

// Helper function for closing PCM transport.
static int close_transport(cdsp_t *pcm) {
  int rv = 0;
  pthread_mutex_lock(&pcm->mutex);
  if (pcm->cdsp_pcm_fd != -1) {
    rv |= close(pcm->cdsp_pcm_fd);
    pcm->cdsp_pcm_fd = -1;
  }
  pthread_mutex_unlock(&pcm->mutex);
  return rv;
}

// Helper function for IO thread termination.
static void io_thread_cleanup(cdsp_t *pcm) {
  debug("IO thread cleanup\n");
  pcm->io_started = false;
}

// Helper function for IO thread delay calculation.
static void io_thread_update_delay(cdsp_t *pcm, snd_pcm_uframes_t hw_ptr) {
  struct timespec now;
  unsigned int nread = 0;

  gettimestamp(&now);
  // Get the number of bytes still in the pipe to cdsp
  ioctl(pcm->cdsp_pcm_fd, FIONREAD, &nread);

  pthread_mutex_lock(&pcm->mutex);

  // stash current time and levels
  pcm->delay_ts = now;
  pcm->delay_pcm_nread = nread;
  if (pcm->io_status < 0) {
    pcm->delay_hw_ptr = 0;
    if (pcm->io.stream == SND_PCM_STREAM_PLAYBACK)
      pcm->delay_running = false;
  }
  else {
    pcm->delay_hw_ptr = hw_ptr;
    if (pcm->io.stream == SND_PCM_STREAM_PLAYBACK)
      pcm->delay_running = true;
  }

  pthread_mutex_unlock(&pcm->mutex);

}

// IO thread, which facilitates ring buffer.
static void *io_thread(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  pthread_cleanup_push(PTHREAD_CLEANUP(io_thread_cleanup), pcm);
  int xrun = 0;

  sigset_t sigset;
  sigemptyset(&sigset);

  // Block signal, which will be used for pause/resume actions.
  sigaddset(&sigset, SIGIO);
  // Block SIGPIPE, so we could receive EPIPE while writing to the pipe
  // whose reading end has been closed. This will allow clean playback
  // termination.
  sigaddset(&sigset, SIGPIPE);

  if ((errno = pthread_sigmask(SIG_BLOCK, &sigset, NULL)) != 0) {
    SNDERR("Thread signal mask error: %s", strerror(errno));
    goto fail;
  }

  // We update pcm->io_hw_ptr (i.e. the value seen by ioplug) only when
  // a period has been completed. We use a temporary copy during the
  // transfer procedure.
  snd_pcm_uframes_t io_hw_ptr = pcm->io_hw_ptr;

  debug("Starting IO loop: %d\n", pcm->cdsp_pcm_fd);
  for (;;) {
    if (pcm->pause_state & CDSP_PAUSE_STATE_PENDING ||
        pcm->io_status < 0) {
      debug("Pausing IO thread\n");

      pthread_mutex_lock(&pcm->mutex);
      pcm->pause_state = CDSP_PAUSE_STATE_PAUSED;
      pthread_cond_signal(&pcm->pause_cond);
      pthread_mutex_unlock(&pcm->mutex);

      int tmp;
      sigwait(&sigset, &tmp);

      pthread_mutex_lock(&pcm->mutex);
      pcm->pause_state = CDSP_PAUSE_STATE_RUNNING;
      pthread_mutex_unlock(&pcm->mutex);

      debug("IO thread resumed\n");

      if (pcm->io_status < 0)
        continue;
      if (pcm->cdsp_pcm_fd == -1) {
        error("FAILING BECAUSE PIPE GONE\n");
        goto fail;
      }
    }

    // There are 2 reasons why the number of available frames may be
    // zero: XRUN or drained final samples; we set the io_status to
    // -1 to indicate we have no work to do.
    snd_pcm_uframes_t avail;
    if ((avail = snd_pcm_ioplug_hw_avail(io, io_hw_ptr, io->appl_ptr)) == 0) {
      if(io->state == SND_PCM_STATE_DRAINING) {
        // Draining is complete.  Signal that to the ioplug code so it will
        // drop the pcm.
        pcm->io_status = -1;
        io_thread_update_delay(pcm, 0);
        eventfd_write(pcm->event_fd, 1);
        continue;
      } else {
        warn("IO Thread out of data.\n");
        // Running and no data is available.  The internal alsa buffer is
        // empty.  This isn't a problem until a period has passed though
        // at which point we have an underrun condition.
        // Sleep in 1/4 period intervals to wait for data to catch up
        // Add 1 extra sample to the period to allow for clock differences
        // and rounding errors
        uint64_t quarter_period_ns = 
          (1000000000 / 4) * (io->period_size+1) / io->rate;
        struct timespec ts;
        ts.tv_sec = quarter_period_ns / 1000000000;
        ts.tv_nsec = quarter_period_ns - 1000000000 * ts.tv_sec;
        debug("Sleep Time: %ld %ld %lu %d\n", ts.tv_sec, ts.tv_nsec,io->period_size, io->rate);
        nanosleep(&ts, NULL);
        xrun++;
        if(xrun > 4) {
          // We've gone longer than a period with no data.
          // The player isn't providing data fast enough.
          error("XRUN OCCURRED!\n");
          // Signal XRUN to the ioplug code
          pcm->io_status = -1;
          io_thread_update_delay(pcm, 0);
          eventfd_write(pcm->event_fd, 1);
        }
        continue;
      }
    }
    // Data available - reset the xrun counter
    xrun = 0;
    pcm->io_status = 0;

    // current offset of the head pointer in the IO buffer
    snd_pcm_uframes_t offset = io_hw_ptr % io->buffer_size;

    // Transfer at most 1 period of frames each iteration
    snd_pcm_uframes_t frames = io->period_size;
    // ... but do not try to transfer more frames than are available in
    // the ring buffer!
    if (frames > avail) {
      frames = avail;
    }

    // Sometimes alsa chooses a buffer size that isn't an integer multiple
    // of the period size.  In that case don't read past the end of the 
    // buffer.
    if (io->buffer_size - offset < frames) {
      frames = io->buffer_size - offset;
    }

    // IO operation size in bytes
    size_t len = frames * pcm->frame_size;
    char *head = pcm->io_hw_buffer + offset * pcm->frame_size;

    // Increment the HW pointer (with boundary wrap)
    io_hw_ptr += frames;
    if (io_hw_ptr >= pcm->io_hw_boundary)
      io_hw_ptr -= pcm->io_hw_boundary;

    ssize_t ret = 0;

    struct timespec tstart,tstop,twrite;
    gettimestamp(&tstart);

    // Perform atomic write - see the explanation above.
    do {
      if ((ret = write(pcm->cdsp_pcm_fd, head, len)) == -1) {
        if (errno == EINTR)
          continue;
        if (errno != EPIPE)
          SNDERR("PCM FIFO write error: %s", strerror(errno));
        goto fail;
      }
      head += ret;
      len -= ret;
    } while (len != 0);
    io_thread_update_delay(pcm, io_hw_ptr);
    // Things tend to run a little smoother if writes take at least
    // some time.  So slow down when the pipe was empty enough that the
    // write was basically instant.
    gettimestamp(&tstop);
    difftimespec(&tstart, &tstop, &twrite);
    double sampletime = (double)frames/(double)io->rate;
    double writetime = (double)twrite.tv_sec + (double)twrite.tv_nsec/1e9;
    double excess = sampletime - writetime;
    if(excess > 0) {
      excess *= 0.5;
      tstop.tv_sec = (time_t)excess;
      excess -= tstop.tv_sec;
      tstop.tv_nsec = (long)(excess*1e9);
      nanosleep(&tstop, NULL);
    }
    excessive("Frames = %lu = %lf secs, Write Time = %lf\n", frames, sampletime, writetime);

    // Make the new HW pointer value visible to the ioplug.
    pcm->io_hw_ptr = io_hw_ptr;

    // Wake application thread if enough space/frames are available
    // to write avail_min frames.  Note that just as we can't read
    // past the end of the hardware buffer the app can't write past
    // it so the metric is distance from the end of the buffer.
    offset = io_hw_ptr % io->buffer_size;
    if(io->buffer_size - offset >= pcm->io_avail_min)
      eventfd_write(pcm->event_fd, 1);
  }

fail:
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
  pthread_cleanup_pop(1);
  close_transport(pcm);
  eventfd_write(pcm->event_fd, 0xDEAD0000);
  pthread_cond_signal(&pcm->pause_cond);
  return NULL;
}

// Wait up to timeout_ms for the child to exit.  Returns 0 once it is gone
// and -ETIMEDOUT if it is still running.
static int wait_camilla(pid_t pid, long timeout_ms) {
  const struct timespec step = {0, 10 * 1000 * 1000};
  for(long waited = 0; ; waited += 10) {
    pid_t r = waitpid(pid, NULL, WNOHANG);
    // ECHILD: already reaped, e.g. when the player ignores SIGCHLD
    if(r == pid || (r == -1 && errno != EINTR))
      return 0;
    if(waited >= timeout_ms)
      return -ETIMEDOUT;
    nanosleep(&step, NULL);
  }
}

// Fill config_out from the config_in template, replacing the tokens with
// the current hw_params.  A NULL value leaves its token untouched.
static int write_config(cdsp_t *pcm, const char *values[4]) {
  const char *tokens[4] = {pcm->format_token, pcm->rate_token,
    pcm->channels_token, pcm->ext_samp_token};
  FILE *cfgin = fopen(pcm->config_in, "r");
  if(!cfgin) {
    SNDERR("Error reading input config file %s: %s", pcm->config_in,
        strerror(errno));
    return -EINVAL;
  }
  FILE *cfgout = fopen(pcm->cargs[1], "w");
  if(!cfgout) {
    SNDERR("Error writing output config file %s: %s", pcm->cargs[1],
        strerror(errno));
    fclose(cfgin);
    return -EINVAL;
  }
  int err = 0;
  char *line = NULL;
  size_t cap = 0;
  while(!err && getline(&line, &cap, cfgin) != -1) {
    // strrep returns its input when there is nothing to replace and a new
    // allocation otherwise.
    char *obuf = line;
    for(size_t t = 0; t < 4 && !err; t++) {
      if(!values[t]) continue;
      char *next = strrep(obuf, tokens[t], values[t]);
      if(!next) {
        err = -ENOMEM;
      } else if(next != obuf) {
        if(obuf != line) free(obuf);
        obuf = next;
      }
    }
    if(!err && fputs(obuf, cfgout) == EOF) err = -EIO;
    if(obuf != line) free(obuf);
  }
  free(line);
  if(!err && ferror(cfgin)) err = -EIO;
  fclose(cfgin);
  if(fclose(cfgout) != 0 && !err) err = -EIO;
  if(err) SNDERR("Error writing output config file %s", pcm->cargs[1]);
  return err;
}

static int start_camilla(cdsp_t *pcm) {
  const char *sformat = cdsp_format_name(pcm->io.format);
  if(!sformat) {
    // Shouldn't get here
    SNDERR("Unsupported Format: %s", snd_pcm_format_name(pcm->io.format));
    return -EINVAL;
  }

  char schannels[10]; // The number of channels should fit in 9 digits
  snprintf(schannels, sizeof(schannels), "%u", pcm->io.channels);

  char srate[10]; // The rate should fit in 9 digits too
  snprintf(srate, sizeof(srate), "%u", pcm->io.rate);

  char sextrasamples[20];  // Some use really long audio chains
  long extrasamples = -1;
  // We multiply the ext_samp by the ratio of sample rate to
  // one of the two common audio rates if the sample rate is an
  // integer multiple
  if((pcm->ext_samp_44100 >= 0) && ((pcm->io.rate % 44100) == 0)) {
    extrasamples = pcm->ext_samp_44100*(pcm->io.rate/44100);
  } else if((pcm->ext_samp_48000 >= 0) && ((pcm->io.rate % 48000) == 0)) {
    extrasamples = pcm->ext_samp_48000*(pcm->io.rate/48000);
  } else if(pcm->ext_samp >= 0) {
    extrasamples = pcm->ext_samp;
  }
  if(extrasamples >= 0) {
    snprintf(sextrasamples, sizeof(sextrasamples), "%ld", extrasamples);
  }

  debug("cpath: %s\n", pcm->cpath);
  debug("config_in: %s\n", pcm->config_in);
  debug("config_out: %s\n", pcm->cargs[1]);
  debug("config_cmd: %s\n", pcm->config_cmd);
  debug("config_cdsp: %ld\n", pcm->config_cdsp);
  debug("vol_file: %s\n", pcm->vol_file);

  double gain = 0;
  // Use mute < 0 as the flag for gain and mute being set.
  int mute = -1;
  if(pcm->vol_file) {
    FILE *volfile = fopen(pcm->vol_file, "r");
    if(!volfile) {
      SNDERR("Error reading input volume file %s", pcm->vol_file);
      return -EINVAL;
    }
    int n = fscanf(volfile, "%lf %d", &gain, &mute);
    fclose(volfile);
    if(n != 2) {
      SNDERR("Error reading input volume file %s", pcm->vol_file);
      return -EINVAL;
    }
    debug("Read Volume File:  Gain = %lf, Mute = %d\n", gain, mute);
  }

  if(pcm->config_in) {
    const char *values[4] = {sformat, srate, schannels,
      extrasamples >= 0 ? sextrasamples : NULL};
    int err = write_config(pcm, values);
    if(err < 0) return err;
  } else if(pcm->config_cmd) {
    char command[1000];
    // Call the config_cmd with the hw params to do whatever
    // camilla configuration is desired
    // Command will be called with arguments "format rate channels"
    snprintf(command, sizeof(command), "%s %s %u %u", pcm->config_cmd,
        sformat, pcm->io.rate, pcm->io.channels);
    debug("Calling config_cmd %s\n", command);
    if(system(command) != 0) {
      SNDERR("Error executing config_cmd %s", pcm->config_cmd);
      return -EINVAL;
    }
  }

  // Only async-signal-safe calls are allowed between fork() and execv()
  // in a multithreaded player, so the argument list is built here.
  char farg[] = "-f";
  char rarg[] = "-r";
  char narg[] = "-n";
  char earg[] = "-e";
  char marg[] = "-m";
  char sgain[30]; // Big enough for argument and value
  char *argv[sizeof(pcm->cargs) / sizeof(pcm->cargs[0])];
  size_t argc = 0;
  for(size_t a = 0; a < pcm->n_cargs; a++) {
    argv[argc++] = pcm->cargs[a];
  }
  if(pcm->config_cdsp) {
    // Pass the hw_params as arguments directly to CamillaDSP
    argv[argc++] = farg;
    argv[argc++] = (char *)sformat;
    argv[argc++] = rarg;
    argv[argc++] = srate;
    argv[argc++] = narg;
    argv[argc++] = schannels;
    if(extrasamples >= 0) {
      argv[argc++] = earg;
      argv[argc++] = sextrasamples;
    }
  }
  if(mute >= 0) {
    snprintf(sgain, sizeof(sgain), "-g%lf", gain);
    argv[argc++] = sgain;
    if(mute > 0) {
      argv[argc++] = marg;
    }
  }
  argv[argc] = NULL;

  // Pipe to send data to camilla.  Close-on-exec keeps the write end out of
  // any other process, so CamillaDSP sees end of file when we close it.
  int fd[2];
  if(pipe2(fd, O_CLOEXEC)) {
    return -errno;
  }
  // Reports an execv failure from the child.  Closed by a successful execv.
  int status_fd[2];
  if(pipe2(status_fd, O_CLOEXEC)) {
    int e = errno;
    close(fd[0]);
    close(fd[1]);
    return -e;
  }

  pid_t cpid = fork();
  if(cpid < 0) {
    int e = errno;
    close(fd[0]);
    close(fd[1]);
    close(status_fd[0]);
    close(status_fd[1]);
    return -e;
  }

  if(cpid == 0) {
    // Child process
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    int e = 0;
    if(fd[0] == STDIN_FILENO) {
      if(fcntl(fd[0], F_SETFD, 0) == -1) e = errno;
    } else if(dup2(fd[0], STDIN_FILENO) == -1) {
      e = errno;
    }
    if(!e) {
      execv(pcm->cpath, argv);
      e = errno;
    }
    ssize_t unused = write(status_fd[1], &e, sizeof(e));
    (void)unused;
    _exit(127);
  }

  // Parent process
  close(fd[0]);
  close(status_fd[1]);
  int child_err = 0;
  ssize_t n;
  do {
    n = read(status_fd[0], &child_err, sizeof(child_err));
  } while(n == -1 && errno == EINTR);
  close(status_fd[0]);
  if(n > 0) {
    SNDERR("Failed to execute CamillaDSP %s: %s", pcm->cpath,
        strerror(child_err));
    close(fd[1]);
    waitpid(cpid, NULL, 0);
    return -ENODEV;
  }
  pcm->cpid = cpid;
  pcm->cdsp_pcm_fd = fd[1];
  return 0;
}

static int cdsp_start(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  debug("Starting\n");

  // If the IO thread is already started, skip thread creation. Otherwise,
  // we might end up with a bunch of IO threads reading or writing to the
  // same FIFO simultaneously. Instead, just send resume signal. */
  if (pcm->io_started) {
    pthread_kill(pcm->io_thread, SIGIO);
    return 0;
  }
  if (pcm->io_joinable) {
      pthread_join(pcm->io_thread, NULL);
      pcm->io_joinable = false;
  }

  // Initialize delay calculation - capture reception begins immediately,
  // playback transmission begins only when first period has been written
  // by the application.
  pcm->delay_running = io->stream == SND_PCM_STREAM_CAPTURE ? true : false;
  gettimestamp(&pcm->delay_ts);

  // start the IO thread
  pcm->io_started = true;
  if ((errno = pthread_create(&pcm->io_thread, NULL,
          PTHREAD_ROUTINE(io_thread), io)) != 0) {
    error("Couldn't create IO thread: %s\n", strerror(errno));
    pcm->io_started = false;
    return -errno;
  }
  pcm->io_joinable = true;

  pthread_setname_np(pcm->io_thread, "pcm-io");
  return 0;
}

static int cdsp_stop(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  debug("Stopping\n");

    // ALSA stops twice on drop + close, and may stop a PCM that never started.
  if (pcm->io_started) {
    pcm->io_started = false;
    pthread_cancel(pcm->io_thread);
  }
  if (pcm->io_joinable) {
      pthread_join(pcm->io_thread, NULL);
      pcm->io_joinable = false;
  }

  pcm->delay_running = false;
  pcm->delay_pcm_nread = 0;

  // Bug in ioplug - if pcm->io_status == -1 then it reports state
  // SND_PCM_STATE_XRUN instead of SND_PCM_STATE_SETUP after PCM
  // was stopped.
  // However -1 should be set if it is due to draining
  if(io->state != SND_PCM_STATE_DRAINING) {
    pcm->io_status = 0;
  }

  // Applications that call poll() after snd_pcm_drain() will be blocked
  // forever unless we generate a poll() event here.
  eventfd_write(pcm->event_fd, 1);

  return 0;
}

static snd_pcm_sframes_t cdsp_pointer(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  
  // Any error returned here is translated to -EPIPE, SND_PCM_STATE_XRUN,
  // by ioplug; and that prevents snd_pcm_readi() and snd_pcm_writei()
  // from returning -ENODEV to the application on device disconnection.
  // Instead, when the device is disconnected, we update the PCM state
  // directly here but we do not return an error code. This ensures that
  // ioplug does not undo that state change. Both snd_pcm_readi() and
  // snd_pcm_writei() return -ENODEV when the PCM state is
  // SND_PCM_STATE_DISCONNECTED after their internal call to
  // snd_pcm_avail_update(), which will be the case when we set it here.
  if (pcm->cdsp_pcm_fd == -1)
    snd_pcm_ioplug_set_state(io, SND_PCM_STATE_DISCONNECTED);
#ifndef SND_PCM_IOPLUG_FLAG_BOUNDARY_WA
  if (pcm->io_status >= 0)
    return pcm->io_hw_ptr % io->buffer_size;
#endif
  if (pcm->io_status >= 0)
    return pcm->io_hw_ptr;
  return pcm->io_status;
}

static void free_cdsp(cdsp_t **pcm) {
  if ((*pcm)->event_fd != -1)
    close((*pcm)->event_fd);
  if((*pcm)->cpath)
    free((void *)(*pcm)->cpath);
  if((*pcm)->config_in)
    free((void *)(*pcm)->config_in);
  for(size_t f = 0; f < (*pcm)->n_cargs; f++) {
    if((*pcm)->cargs[f] != 0) {
      free((void *)(*pcm)->cargs[f]);
    }
  }
  if((*pcm)->config_cmd)
    free((void *)(*pcm)->config_cmd);
  if((*pcm)->vol_file)
    free((void *)(*pcm)->vol_file);
  if((*pcm)->format_token)
    free((void *)(*pcm)->format_token);
  if((*pcm)->rate_token)
    free((void *)(*pcm)->rate_token);
  if((*pcm)->channels_token)
    free((void *)(*pcm)->channels_token);
  if((*pcm)->ext_samp_token)
    free((void *)(*pcm)->ext_samp_token);
  if((*pcm)->start_cmd)
    free((void *)(*pcm)->start_cmd);
  if((*pcm)->camilla_exit_cmd)
    free((void *)(*pcm)->camilla_exit_cmd);
  pthread_mutex_destroy(&(*pcm)->mutex);
  pthread_cond_destroy(&(*pcm)->pause_cond);
  free((void *)*pcm);
}

static int cdsp_close(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  debug("Closing\n");
  free_cdsp(&pcm);
  return 0;
}

static int cdsp_hw_free(snd_pcm_ioplug_t *io);

static int cdsp_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params __attribute__((unused))) {
  cdsp_t *pcm = io->private_data;
  info("Initializing hw_params: %s %d %d\n",
      snd_pcm_format_name(io->format), io->rate, io->channels);

  // Only one CamillaDSP can hold the output device at a time.
  if(pcm->cpid != -1)
    cdsp_hw_free(io);

  pcm->frame_size = (snd_pcm_format_physical_width(io->format)*io->channels)/8;

  // Start CamillaDSP in a forked process
  int err = start_camilla(pcm);
  if(err < 0)
    return err;

  // The pipe size is set by the fifo_size option.  A small pipe keeps the
  // delay low, a large one gives CamillaDSP more slack when it reads large
  // chunks.  The kernel rounds it up to a power of two pages and limits it
  // to /proc/sys/fs/pipe-max-size.
  int fifo_bytes = fcntl(pcm->cdsp_pcm_fd, F_SETPIPE_SZ, (int)pcm->fifo_size);
  if(fifo_bytes < 0) {
    SNDERR("Couldn't set FIFO size to %ld bytes: %s", pcm->fifo_size,
        strerror(errno));
    fifo_bytes = fcntl(pcm->cdsp_pcm_fd, F_GETPIPE_SZ);
  }
  pcm->delay_fifo_size = fifo_bytes / (snd_pcm_sframes_t)pcm->frame_size;

  info("FIFO buffer size: %ld frames\n", pcm->delay_fifo_size);

  /* ALSA default for avail min is one period. */
  pcm->io_avail_min = io->period_size;

  info("Selected HW buffer: %ld periods x %ld bytes %c= %ld bytes\n",
      io->buffer_size / io->period_size, pcm->frame_size * io->period_size,
      io->period_size * (io->buffer_size / io->period_size) == io->buffer_size ? '=' : '<',
      io->buffer_size * pcm->frame_size);

  return 0;
}

static int cdsp_hw_free(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;
  debug("Freeing HW\n");
  int err = 0;
  if(pcm->camilla_exit_cmd) {
    debug("Calling camilla_exit_cmd: %s\n", pcm->camilla_exit_cmd);
    // Call the camilla_exit_cmd 
    err = system(pcm->camilla_exit_cmd);
    if(err != 0) {
      SNDERR("Error executing camilla_exit_cmd %s\n", pcm->camilla_exit_cmd);
    }
  } 
  debug("Stopping Camilla\n");
  if(pcm->kill_on_drop && !pcm->drained && pcm->cpid != -1) {
    // Dropped: what is still in the pipe and in CamillaDSP is not wanted.
    debug("Dropped, terminating Camilla\n");
    kill(pcm->cpid, SIGTERM);
    if(wait_camilla(pcm->cpid, 500) < 0) {
      kill(pcm->cpid, SIGKILL);
      waitpid(pcm->cpid, NULL, 0);
    }
    pcm->cpid = -1;
  }
  int close_err = close_transport(pcm) == -1 ? -errno : 0;
  if(pcm->cpid != -1) {
    // Wait on CamillaDSP to finish.  It needs to free the ALSA
    // device before another copy is started.  After the pipe is closed it
    // still plays its extra samples, so give it stop_timeout ms before
    // forcing it.
    if(wait_camilla(pcm->cpid, pcm->stop_timeout) < 0) {
      SNDERR("CamillaDSP did not exit within %ld ms, terminating it",
          pcm->stop_timeout);
      kill(pcm->cpid, SIGTERM);
      if(wait_camilla(pcm->cpid, 2000) < 0) {
        kill(pcm->cpid, SIGKILL);
        waitpid(pcm->cpid, NULL, 0);
      }
    }
    pcm->cpid = -1;
  }
  if(close_err < 0) return close_err;
  if(err > 0) return -err;
  return err;
}

// A check that get_boundary still works
static snd_pcm_uframes_t calc_boundary_size(snd_pcm_ioplug_t *io) {
  snd_pcm_uframes_t boundary;
  boundary = io->buffer_size;
  while (boundary * 2 <= LONG_MAX - io->buffer_size)
    boundary *= 2;
  return boundary;
}

static int cdsp_sw_params(snd_pcm_ioplug_t *io, snd_pcm_sw_params_t *params) {
  cdsp_t *pcm = io->private_data;
  debug("Initializing SW\n");

  snd_pcm_sw_params_get_boundary(params, &pcm->io_hw_boundary);
  assert(pcm->io_hw_boundary == calc_boundary_size(io));

  // We would get avail_min here but alsa has hidden it from the plugin
  // So we'll just have to ignore the player's request and stick to
  // period_size

  return 0;
}

static int cdsp_prepare(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;

  // if PCM FIFO is not opened, report it right away
  if (pcm->cdsp_pcm_fd == -1)
    return -ENODEV;

  // initialize ring buffer and status
  pcm->io_hw_ptr = 0;
  pcm->io_status = 0;
  pcm->drained = false;

  // The ioplug allocates and configures its channel area buffer when the
  // HW parameters are fixed, but after calling cdsp_hw_params(). So,
  // this is the earliest opportunity for us to safely cache the ring
  // buffer start address.
  const snd_pcm_channel_area_t *areas = snd_pcm_ioplug_mmap_areas(io);
  pcm->io_hw_buffer = (char *)areas->addr + areas->first / 8;

  // Indicate that our PCM is ready for IO, even though is is not 100%
  // true - the IO thread may not be running yet. Applications using
  // snd_pcm_sw_params_set_start_threshold() require the PCM to be usable
  // as soon as it has been prepared.
  pcm->first_revent = true;
  eventfd_write(pcm->event_fd, 1);

  debug("Prepared\n");
  return 0;
}

static int cdsp_drain(snd_pcm_ioplug_t *io) {
  debug("Draining\n");
  cdsp_t *pcm = io->private_data;
  // ioplug does not start a PCM that is drained while still prepared (less
  // than a start threshold was written) when the plugin has its own drain.
  if(!pcm->io_started && cdsp_start(io) < 0)
    return 0;
  // Wait for the playback thread to empty the ring buffer
  while(pcm->io_status >= 0 && pcm->io_started) {
    usleep(10);
  }
  pcm->drained = true;
  // We cannot recover from an error here. By returning zero we ensure that
  // ioplug stops the pcm. Returning an error code would be interpreted by
  // ioplug as an incomplete drain and would it leave the pcm running.
  return 0;
}

// Calculate overall PCM delay.
//
// Exact calculation of the PCM delay is very hard, if not impossible. For
// the sake of simplicity we will make few assumptions and approximations.
// In general, the delay of this plugin is proportional to the number of 
// bytes queued in the FIFO buffer.  Of course CamillaDSP may add consdirable
// additional delay which is not accounted for in this estimation.
static snd_pcm_sframes_t cdsp_calculate_delay(snd_pcm_ioplug_t *io) {
  cdsp_t *pcm = io->private_data;

  snd_pcm_sframes_t delay = 0;

  struct timespec now;

  pthread_mutex_lock(&pcm->mutex);

  gettimestamp(&now);
  struct timespec diff;
  difftimespec(&now, &pcm->delay_ts, &diff);

  // the maximum number of frames that can have been
  // produced/consumed by the server since pcm->delay_ts
  unsigned int tframes =
    //(diff.tv_sec * 1000 + diff.tv_nsec / 1000000) * io->rate / 1000;
    (unsigned int)(((double)diff.tv_sec + ((double)diff.tv_nsec)/1e9) * io->rate);

  // the number of frames that were in the FIFO at pcm->delay_ts
  snd_pcm_uframes_t fifo_delay = pcm->delay_pcm_nread / pcm->frame_size;

  delay = fifo_delay;

  // The buffer_delay is the number of frames that were in the buffer at
  // pcm->delay_ts, adjusted the number written by the application since
  // then.
  snd_pcm_sframes_t buffer_delay = 0;
  if (io->state != SND_PCM_STATE_XRUN)
    buffer_delay = snd_pcm_ioplug_hw_avail(io, pcm->delay_hw_ptr, io->appl_ptr);
  delay += buffer_delay;

  if (pcm->delay_running) {
    // Adjust the total delay by the number of frames consumed.
    if ((delay -= tframes) < 0) delay = 0;
  }

  pthread_mutex_unlock(&pcm->mutex);

  return delay;
}

static int cdsp_pause(snd_pcm_ioplug_t *io, int enable) {
  cdsp_t *pcm = io->private_data;

  if (enable == 1) {
    // Synchronize the IO thread with an application thread to ensure that
    // the server will not be paused while we are processing a transfer.
    pthread_mutex_lock(&pcm->mutex);
    pcm->pause_state |= CDSP_PAUSE_STATE_PENDING;
    while (!(pcm->pause_state & CDSP_PAUSE_STATE_PAUSED) 
        && pcm->cdsp_pcm_fd != -1) {
      pthread_cond_wait(&pcm->pause_cond, &pcm->mutex);
    }
    pthread_mutex_unlock(&pcm->mutex);
  }

  if (enable == 0) {
    pcm->first_revent = true;
    if (pcm->io_started)
      pthread_kill(pcm->io_thread, SIGIO);
  } else {
    // store current delay value
    pcm->delay_paused = cdsp_calculate_delay(io);
  }

  // Even though PCM transport is paused, our IO thread is still running. If
  // the implementer relies on the PCM file descriptor readiness, we have to
  // bump our internal event trigger. Otherwise, client might stuck forever
  // in the poll/select system call.
  eventfd_write(pcm->event_fd, 1);

  return 0;
}

static void cdsp_dump(snd_pcm_ioplug_t *io, snd_output_t *out) {
  cdsp_t *pcm = io->private_data;
  snd_output_printf(out, "CamillaDSP Plugin\n");
  snd_output_printf(out, "c_path: %s\n", pcm->cpath);
  snd_output_printf(out, "config_out: %s\n", pcm->cargs[1]);
  if(pcm->config_in)
    snd_output_printf(out, "config_in: %s\n", pcm->config_in);
  if(pcm->config_cmd)
    snd_output_printf(out, "config_cmd: %s\n", pcm->config_cmd);
  snd_output_printf(out, "vol_file: %s\n", pcm->vol_file);
  // alsa-lib commits the PCM setup only if cdsp_hw_params() returned
  // success, so we only dump the ALSA PCM parameters if CamillaDSP was
  // started.
  if (pcm->cpid >= 0) {
    snd_output_printf(out, "Its setup is:\n");
    snd_pcm_dump_setup(io->pcm, out);
  }
}

static int cdsp_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp) {
  cdsp_t *pcm = io->private_data;

  if (pcm->cdsp_pcm_fd == -1)
    return -ENODEV;

  int ret = 0;
  *delayp = 0;

  switch (io->state) {
    case SND_PCM_STATE_PREPARED:
    case SND_PCM_STATE_RUNNING:
      *delayp = cdsp_calculate_delay(io);
      break;
    case SND_PCM_STATE_PAUSED:
      *delayp = pcm->delay_paused;
      break;
    case SND_PCM_STATE_XRUN:
      *delayp = cdsp_calculate_delay(io);
      ret = -EPIPE;
      break;
    case SND_PCM_STATE_SUSPENDED:
      ret = -ESTRPIPE;
      break;
    case SND_PCM_STATE_DISCONNECTED:
      ret = -ENODEV;
      break;
    default:
      break;
  }

  return ret;
}

static int cdsp_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd,
    unsigned int nfds, unsigned short *revents) {
  cdsp_t *pcm = io->private_data;

  *revents = 0;
  int ret = 0;

  if (pcm->cdsp_pcm_fd == -1)
    goto fail;

  // We only advertise a single file descriptor so the 
  // player really should be giving us that descriptor
  // and just that descriptor.  
  assert(nfds == 1);
  assert(pfd[0].fd == pcm->event_fd);

  if (pfd[0].revents & POLLIN) {

    eventfd_t event;
    eventfd_read(pcm->event_fd, &event);

    if (event & 0xDEAD0000)
      goto fail;

    // This call synchronizes the ring buffer pointers and updates the
    // ioplug state.
    snd_pcm_sframes_t avail = snd_pcm_avail(io->pcm);

    // ALSA expects that the event will match stream direction, e.g.
    // playback will not start if the event is for reading.
    *revents = io->stream == SND_PCM_STREAM_CAPTURE ? POLLIN : POLLOUT;

    // We hold the event fd ready, unless insufficient frames are
    // available in the ring buffer.
    bool ready = true;

    switch (io->state) {
      case SND_PCM_STATE_SETUP:
        ready = false;
        *revents = 0;
        break;
      case SND_PCM_STATE_PREPARED:
        // capture poll should block forever
        if (io->stream == SND_PCM_STREAM_CAPTURE) {
          ready = false;
          *revents = 0;
        }
        if ((snd_pcm_uframes_t)avail < pcm->io_avail_min) {
          ready = false;
          *revents = 0;
        }
        break;
      case SND_PCM_STATE_RUNNING:
        if ((snd_pcm_uframes_t)avail < pcm->io_avail_min) {
          if(pcm->first_revent) {
            pcm->first_revent = false;
          } else {
            warn("Revents overcall %lu < %lu\n", avail, pcm->io_avail_min);
          }
          *revents = 0;
        }
        ready = false;
        break;
      case SND_PCM_STATE_XRUN:
      case SND_PCM_STATE_PAUSED:
      case SND_PCM_STATE_SUSPENDED:
        *revents |= POLLERR;
        break;
      case SND_PCM_STATE_DISCONNECTED:
        *revents = POLLERR;
        ret = -ENODEV;
        break;
      case SND_PCM_STATE_OPEN:
        *revents = POLLERR;
        ret = -EBADF;
        break;
      default:
        break;
    };

    if (ready)
      eventfd_write(pcm->event_fd, 1);

  }

  return ret;

fail:
  *revents = POLLERR | POLLHUP;
  return -ENODEV;
}

static const snd_pcm_ioplug_callback_t cdsp_callback = {
  .start = cdsp_start,
  .stop = cdsp_stop,
  .pointer = cdsp_pointer,
  .close = cdsp_close,
  .hw_params = cdsp_hw_params,
  .hw_free = cdsp_hw_free,
  .sw_params = cdsp_sw_params,
  .prepare = cdsp_prepare,
  .drain = cdsp_drain,
  .pause = cdsp_pause,
  .poll_revents = cdsp_poll_revents,
  .dump = cdsp_dump,
  .delay = cdsp_delay,
};

// THIS ASSUMES SRC IS NULL TERMINATED!
static int alloc_copy_string(char **dst, const char *src) {
  size_t len = strlen(src)+1;
  *dst = (char *)malloc(len);
  if(!(*dst)) {
    SNDERR("Out of memory");
    return -ENOMEM;
  }
  strncpy(*dst, src, len);
  return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(cdsp) {
  debug("Plugin creation\n");

  cdsp_t *pcm;
  int err = 0;
  snd_config_iterator_t i, next;
  const char *temp = NULL;
  pcm = calloc(1, sizeof(*pcm));
  if(pcm == NULL) {
    return -ENOMEM;
  }

  // HW Parameters to accept
  long min_channels = 0;
  long max_channels = 0;
  long channels = 0;
  // If they list more than 100 rates too bad.
  unsigned int n_rates = 0;
  pcm->n_cargs = 2; // First two are proc name and config
  unsigned int rate_list[100];
  long min_rate = 0;
  long max_rate = 0;
  unsigned int format_list[CDSP_N_FORMATS] = {
    SND_PCM_FORMAT_S16_LE,
    SND_PCM_FORMAT_S24_LE,
    SND_PCM_FORMAT_S24_3LE,
    SND_PCM_FORMAT_S32_LE,
    SND_PCM_FORMAT_FLOAT_LE,
    SND_PCM_FORMAT_FLOAT64_LE
  };
  unsigned int n_formats = CDSP_N_FORMATS;
  if((err = alloc_copy_string(&pcm->cargs[0], "camilladsp")) < 0) goto _err;
  pcm->ext_samp = -1;
  pcm->ext_samp_44100 = -1;
  pcm->ext_samp_48000 = -1;
  pcm->fifo_size = 2048;
  pcm->stop_timeout = 10000;

  snd_config_for_each(i, next, conf) {
    snd_config_t *n = snd_config_iterator_entry(i);
    const char *id;
    if(snd_config_get_id(n, &id) < 0)
      continue;
    if(strcmp(id, "comment") == 0 || strcmp(id, "type") == 0 ||
        strcmp(id, "hint") == 0)
      continue;
    if(strcmp(id, "formats") == 0) {
      n_formats = 0;
      snd_config_iterator_t fi, fnext;
      snd_config_for_each(fi, fnext, n) {
        if(n_formats >= CDSP_N_FORMATS) {
          SNDERR("Too many formats specified.  Max %d.", CDSP_N_FORMATS);
          err = -EINVAL;
          goto _err;
        }
        snd_config_t *fn = snd_config_iterator_entry(fi);
        if((err = snd_config_get_string(fn, &temp)) < 0) goto _err;
        snd_pcm_format_t format = snd_pcm_format_value(temp);
        if(!cdsp_format_name(format)) {
          SNDERR("Unsupported format %s.  Use S16_LE, S24_LE, S24_3LE, "
              "S32_LE, FLOAT_LE or FLOAT64_LE.", temp);
          err = -EINVAL;
          goto _err;
        }
        format_list[n_formats++] = format;
      }
      if(n_formats == 0) {
        SNDERR("formats must list at least one format.");
        err = -EINVAL;
        goto _err;
      }
      continue;
    }
    if(strcmp(id, "fifo_size") == 0) {
      if((err = snd_config_get_integer(n, &pcm->fifo_size)) < 0) goto _err;
      if(pcm->fifo_size <= 0 || pcm->fifo_size > INT_MAX) {
        SNDERR("fifo_size must be > 0");
        err = -EINVAL;
        goto _err;
      }
      continue;
    }
    if(strcmp(id, "stop_timeout") == 0) {
      if((err = snd_config_get_integer(n, &pcm->stop_timeout)) < 0)
        goto _err;
      if(pcm->stop_timeout < 0) {
        SNDERR("stop_timeout must be >= 0");
        err = -EINVAL;
        goto _err;
      }
      continue;
    }
    if(strcmp(id, "kill_on_drop") == 0) {
        if((err = snd_config_get_bool(n)) < 0) goto _err;
        pcm->kill_on_drop = err;
        err = 0;
        continue;
    }
    if(strcmp(id, "cpath") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->cpath, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "config_in") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->config_in, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "config_out") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->cargs[1], temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "config_cmd") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->config_cmd, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "config_cdsp") == 0) {
      if((err = snd_config_get_integer(n, &pcm->config_cdsp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "vol_file") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->vol_file, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "format_token") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->format_token, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "samplerate_token") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->rate_token, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "channels_token") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->channels_token, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "extrasamples_token") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->ext_samp_token, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "cargs") == 0) {
      snd_config_iterator_t ci, cnext;
      snd_config_for_each(ci, cnext, n) {
        if(pcm->n_cargs >= 100) {
          SNDERR("Too many args specified.  Max 100.");
          err = -1;
          goto _err;
        }
        snd_config_t *cn = snd_config_iterator_entry(ci);
        if((err = snd_config_get_string(cn, &temp)) < 0) goto _err;
        if((err = alloc_copy_string(&pcm->cargs[pcm->n_cargs], temp)) < 0)
          goto _err;
        pcm->n_cargs++;
      }
      continue;
    }
    if(strcmp(id, "rates") == 0) {
      snd_config_iterator_t ri, rnext;
      snd_config_for_each(ri, rnext, n) {
        if(n_rates >= 100) {
          SNDERR("Too many rates specified.  Max 100.");
          err = -1;
          goto _err;
        }
        snd_config_t *rn = snd_config_iterator_entry(ri);
        long rate;
        if((err = snd_config_get_integer(rn, &rate)) < 0) goto _err;
        rate_list[n_rates++] = (unsigned)rate;
      }
      continue;
    }
    if(strcmp(id, "min_rate") == 0) {
      if((err = snd_config_get_integer(n, &min_rate)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "max_rate") == 0) {
      if((err = snd_config_get_integer(n, &max_rate)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "min_channels") == 0) {
      if((err = snd_config_get_integer(n, &min_channels)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "max_channels") == 0) {
      if((err = snd_config_get_integer(n, &max_channels)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "channels") == 0) {
      if((err = snd_config_get_integer(n, &channels)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "extra_samples") == 0) {
      if((err = snd_config_get_integer(n, &pcm->ext_samp)) < 0) goto _err;
      if(pcm->ext_samp < 0) {
        SNDERR("extra_samples must be >= 0");
        err = -EINVAL;
        goto _err;
      }
      continue;
    }
    if(strcmp(id, "extra_samples_44100") == 0) {
      if((err = snd_config_get_integer(n, &pcm->ext_samp_44100)) < 0) 
        goto _err;
      if(pcm->ext_samp_44100 < 0) {
        SNDERR("extra_samples_44100 must be >= 0");
        err = -EINVAL;
        goto _err;
      }
      continue;
    }
    if(strcmp(id, "extra_samples_48000") == 0) {
      if((err = snd_config_get_integer(n, &pcm->ext_samp_48000)) < 0) 
        goto _err;
      if(pcm->ext_samp_48000 < 0) {
        SNDERR("extra_samples_48000 must be >= 0");
        err = -EINVAL;
        goto _err;
      }
      continue;
    }
    if(strcmp(id, "start_cmd") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->start_cmd, temp)) < 0) goto _err;
      continue;
    }
    if(strcmp(id, "camilla_exit_cmd") == 0) {
      if((err = snd_config_get_string(n, &temp)) < 0) goto _err;
      if((err = alloc_copy_string(&pcm->camilla_exit_cmd, temp)) < 0) goto _err;
      continue;
    }
    err = -EINVAL;
    goto _err;
  }
  
  // Validate user input
  if(!pcm->cpath) {
    SNDERR("Must supply cpath parameter with path to CamillaDSP.");
    err = -EINVAL;
    goto _err;
  }
  if(!pcm->config_in && !pcm->config_cmd && !pcm->config_cdsp) {
    SNDERR("Must supply config_in, config_cmd, or config_cdsp parameter.");
    err = -EINVAL;
    goto _err;
  }
  if(pcm->config_in) {
    if(pcm->config_cmd || pcm->config_cdsp) {
      SNDERR("Only config_in, config_cmd, or config_cdsp can be set.");
      err = -EINVAL;
      goto _err;
    }
  } else if(pcm->config_cmd) {
    if(pcm->config_cdsp) {
      SNDERR("Only config_in, config_cmd, or config_cdsp can be set.");
      err = -EINVAL;
      goto _err;
    }
  }

  if(!pcm->cargs[1]) {
    SNDERR("Must supply config_out file parameter.");
    err = -EINVAL;
    goto _err;
  }

  if(channels == 0) {
    if(min_channels <= 0 || max_channels <= 0) {
      SNDERR("Must supply valid channel information.");
      err = -EINVAL;
      goto _err;
    }
    if(min_channels > max_channels) {
      SNDERR("Max channels must be >= min channels.");
      err = -EINVAL;
      goto _err;
    }
  } else {
    if(min_channels != 0 || max_channels != 0) {
      SNDERR("Cannot set channels and min/max channels.");
      err = -EINVAL;
      goto _err;
    }
    if(channels < 0) {
      SNDERR("Must supply valid channel information.");
      err = -EINVAL;
      goto _err;
    }
    min_channels = max_channels = channels;
  }

  if(n_rates == 0) {
    if(min_rate <= 0 || max_rate <= 0) {
      SNDERR("Must supply valid sample rate information.");
      err = -EINVAL;
      goto _err;
    }
    if(min_rate > max_rate) {
      SNDERR("Max sample rate must be >= min sample rate.");
      err = -EINVAL;
      goto _err;
    }
  } else {
    if(min_rate != 0 || max_rate != 0) {
      SNDERR("Cannot set rates and min/max rates.");
      err = -EINVAL;
      goto _err;
    }
    // Now find the max rate in the list for setting the period size
    // below
    max_rate = rate_list[0];
    for(unsigned int ii = 0; ii < n_rates; ii++) {
      if(rate_list[ii] > max_rate) max_rate = rate_list[ii];
    }
  }
  if(pcm->config_in) {
    if(!pcm->format_token) {
      if((err = alloc_copy_string(&pcm->format_token, "$format$")) < 0) 
        goto _err;
    }
    if(!pcm->rate_token) {
      if((err = alloc_copy_string(&pcm->rate_token, "$samplerate$")) < 0) 
        goto _err;
    }
    if(!pcm->channels_token) {
      if((err = alloc_copy_string(&pcm->channels_token, "$channels$")) < 0) 
        goto _err;
    }
    if(!pcm->ext_samp_token) {
      if((err = alloc_copy_string(&pcm->ext_samp_token, "$extrasamples$")) < 0) 
        goto _err;
    }
  }
    
  // Done parsing / validating user input


  // Establish the event_fd used to signal ALSA
  if ((pcm->event_fd = eventfd(0, EFD_CLOEXEC)) == -1) {
    err = -errno;
    goto _err;
  }

  pcm->io.version = SND_PCM_IOPLUG_VERSION;
  pcm->io.name = "CamillaDSP Plugin";
  pcm->io.callback = &cdsp_callback;
  pcm->io.private_data = pcm;
  pcm->io.poll_events = POLLIN;
  pcm->io.poll_fd = pcm->event_fd;
  pcm->cpid = -1;
  pcm->cdsp_pcm_fd = -1;
  pthread_mutex_init(&pcm->mutex, NULL);
  pthread_cond_init(&pcm->pause_cond, NULL);
  pcm->pause_state = CDSP_PAUSE_STATE_RUNNING;
  pcm->io.flags = SND_PCM_IOPLUG_FLAG_LISTED;
#ifdef SND_PCM_IOPLUG_FLAG_BOUNDARY_WA
  pcm->io.flags |= SND_PCM_IOPLUG_FLAG_BOUNDARY_WA;
#endif
  pcm->io.mmap_rw = 1;

#if SND_LIB_VERSION >= 0x010102 && SND_LIB_VERSION <= 0x010103
  /* ALSA library thread-safe API functionality does not play well with ALSA
   * IO-plug plug-ins. It causes deadlocks which often make our PCM plug-in
   * unusable. As a workaround we are going to disable this functionality. */
  if (setenv("LIBASOUND_THREAD_SAFE", "0", 0) == -1)
    SNDERR("Couldn't disable ALSA thread-safe API: %s", strerror(errno));
#endif
  err = snd_pcm_ioplug_create(&pcm->io, name, stream, mode);
  if(err < 0) goto _err;
  
  // Configure "hw" constraints
  if((err = snd_pcm_ioplug_set_param_list(&pcm->io, 
      SND_PCM_IOPLUG_HW_FORMAT, n_formats, format_list)) < 0) goto _err;

  if((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
      SND_PCM_IOPLUG_HW_CHANNELS, min_channels, max_channels)) < 0) goto _err;

  if(n_rates > 0) {
    if((err = snd_pcm_ioplug_set_param_list(&pcm->io, 
        SND_PCM_IOPLUG_HW_RATE, n_rates, rate_list)) < 0) goto _err;
  } else {
    if((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
        SND_PCM_IOPLUG_HW_RATE, min_rate, max_rate)) < 0) goto _err;
  }

  if ((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
          SND_PCM_IOPLUG_HW_PERIODS, 2, 1024)) < 0)
    goto _err;

  // In order to prevent audio tearing and minimize CPU utilization, we're
  // going to setup period size constraint. The limit is derived from the
  // maximum sampling rate, max channels, and maximum integer format size
  // (32 bits) so the minium period "time" size will be about 10ms. The upper
  // limit will not be constrained.
  unsigned int min_p = max_rate / 100 * max_channels * 4;

  if ((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
          SND_PCM_IOPLUG_HW_PERIOD_BYTES, min_p, 1024 * 1024)) < 0) goto _err;

  unsigned int max_buffer = 2*1024*1024;
  if(max_buffer < 2*min_p) max_buffer = 2*min_p;
  if((err = snd_pcm_ioplug_set_param_minmax(&pcm->io, 
          SND_PCM_IOPLUG_HW_BUFFER_BYTES, 2*min_p, max_buffer)) < 0) goto _err;

  *pcmp = pcm->io.pcm;

  if(pcm->start_cmd) {
    debug("Calling start_cmd: %s\n", pcm->start_cmd);
    // Call the start_cmd 
    int err = system(pcm->start_cmd);
    if(err != 0) {
      SNDERR("Error executing start_cmd %s\n", pcm->start_cmd);
      if(err > 0) return -err;
      return err;
    }
  } 

  return 0;

_err:
  free_cdsp(&pcm);
  return err;
}
SND_PCM_PLUGIN_SYMBOL(cdsp)

