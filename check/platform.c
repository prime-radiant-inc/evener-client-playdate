/* SDK boundary for opt-in host lifecycle checks. */
#include "platform.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

Configuration config;
bool fail_write, fail_rename, fail_flush, fail_unlink;
static SDFile *file_open(const char *name, FileOptions mode) { return fopen(name, mode & kFileWrite ? "wb" : "rb"); }
static int file_close(SDFile *file) { return fclose(file); }
static int file_read(SDFile *file, void *buf, unsigned n) { return (int)fread(buf, 1, n, file); }
static int file_write(SDFile *file, const void *buf, unsigned n) { return (int)fwrite(buf, 1, fail_write ? n / 2 : n, file); }
static int file_flush(SDFile *file) { return fail_flush ? -1 : fflush(file); }
static int file_seek(SDFile *file, int pos, int whence) { return fseek(file, pos, whence); }
static int file_stat(const char *path, FileStat *out) {
    struct stat value; if (stat(path, &value)) return -1;
    memset(out, 0, sizeof(*out)); out->size = (unsigned)value.st_size; return 0;
}
static int file_unlink(const char *name, int recursive) { (void)recursive; return fail_unlink ? -1 : unlink(name); }
static int file_rename(const char *from, const char *to) { return fail_rename ? -1 : rename(from, to); }
static const char *file_error(void) { return "injected storage failure"; }
static void log_message(const char *format, ...) { (void)format; }
static struct playdate_file files = {
    .open = file_open, .close = file_close, .read = file_read, .write = file_write,
    .flush = file_flush, .seek = file_seek, .stat = file_stat, .unlink = file_unlink,
    .rename = file_rename, .geterr = file_error
};
RecordCallback *mic_callback;
AccessRequestCallback *mic_permission;
void *mic_context, *permission_context;
float fixture_time;
unsigned mic_starts;
static float elapsed(void) { return fixture_time; }
static int microphone(RecordCallback *callback, void *context, enum MicSource source) {
    (void)source; mic_callback = callback; mic_context = context;
    if (callback) mic_starts++;
    return kMicInputInternal;
}
static enum accessReply mic_access(const char *purpose, AccessRequestCallback *callback, void *context) {
    (void)purpose; mic_permission = callback; permission_context = context; return kAccessAsk;
}
static struct playdate_sound sound_api = {.setMicCallback = microphone, .requestMicAccess = mic_access};
static struct playdate_sys system_api = {.logToConsole = log_message, .getElapsedTime = elapsed};
static enum accessReply network_access(const char *host, int port, bool tls, const char *purpose, AccessRequestCallback *callback, void *context) {
    (void)host; (void)port; (void)tls; (void)purpose; (void)callback; (void)context; return kAccessAllow;
}
static struct playdate_tcp tcp_api = {.requestAccess = network_access};
static struct playdate_network network_api = {.tcp = &tcp_api};
static PlaydateAPI api = {.file = &files, .system = &system_api, .sound = &sound_api, .network = &network_api};
PlaydateAPI *pd = &api;
void randombytes(unsigned char *bytes, unsigned long long count) { (void)bytes; (void)count; abort(); }
PairingSettings destination(const char *host) {
    PairingSettings out = {.port = 9180, .tailnet = true};
    snprintf(out.host, sizeof(out.host), "%s", host);
    strcpy(out.path, "/rpc"); strcpy(out.token, "synthetic-token"); return out;
}
void bundle(const PairingSettings *s) {
    memset(&config, 0, sizeof(config));
    strcpy(config.hub_host, s->host); strcpy(config.hub_path, s->path); strcpy(config.hub_token, s->token);
    config.hub_port = s->port; config.hub_tls = s->tls; config.use_tailnet = s->tailnet;
    strcpy(config.openai_key, "synthetic-embedded-key"); strcpy(config.seed, "synthetic-build-seed");
}
