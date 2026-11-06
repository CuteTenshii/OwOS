/* aac.c — AAC (Advanced Audio Coding) command-line tool.
 *
 *   aac info   <file.aac|.m4a>          print the stream parameters
 *   aac decode <in.aac|.m4a> <out.wav>  decode AAC-LC to a 16-bit PCM WAV
 *
 * The decoder lives in user/lib/aac; this front end adds the two ways AAC is
 * carried — a raw ADTS elementary stream (.aac) and an MP4/.m4a container
 * (parsed by user/lib/mp4) — and a minimal WAV writer. The whole input is read
 * into memory (the MP4 demuxer needs random access) and the decoded PCM is
 * buffered so the WAV header can carry an exact length; this is a converter for
 * clips, not a streaming player (that path is user/lib/media).
 */
#include "aac.h"
#include "mp4.h"
#include "owos.h"

static void put_le16(unsigned char *p, unsigned v)
{
  p[0] = v;
  p[1] = v >> 8;
}
static void put_le32(unsigned char *p, unsigned v)
{
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

static int tag4(const unsigned char *p, const char *s)
{
  return p[0] == s[0] && p[1] == s[1] && p[2] == s[2] && p[3] == s[3];
}

/* Slurp a whole file into a fresh buffer; *len set on success. */
static unsigned char *slurp(const char *path, unsigned *len)
{
  struct statinfo st;
  unsigned char *b;
  unsigned p = 0;
  if (stat(path, &st) != 0 || st.is_dir)
    return 0;
  b = malloc(st.size ? st.size : 1);
  if (!b)
    return 0;
  while (p < st.size) {
    int n = readfile_at(path, p, b + p, st.size - p);
    if (n <= 0)
      break;
    p += (unsigned)n;
  }
  if (p != st.size) {
    free(b);
    return 0;
  }
  *len = st.size;
  return b;
}

/* A growable interleaved-int16 PCM sink. */
struct pcmbuf {
  short *data;
  unsigned len, cap;
};

static int pcm_append(struct pcmbuf *pb, const short *s, unsigned n)
{
  if (pb->len + n > pb->cap) {
    unsigned nc = (pb->cap ? pb->cap * 2 : 1 << 16);
    while (nc < pb->len + n)
      nc *= 2;
    short *nd = realloc(pb->data, nc * sizeof(short));
    if (!nd)
      return -1;
    pb->data = nd;
    pb->cap = nc;
  }
  memcpy(pb->data + pb->len, s, n * sizeof(short));
  pb->len += n;
  return 0;
}

static int is_adts(const unsigned char *b, unsigned len) { return len >= 2 && b[0] == 0xFF && (b[1] & 0xF6) == 0xF0; }
static int is_mp4(const unsigned char *b, unsigned len) { return len >= 8 && tag4(b + 4, "ftyp"); }

/* Decode every access unit into `pb`; report sample rate + channels. 0 on
 * success. */
static int decode_all(const unsigned char *buf, unsigned len, struct pcmbuf *pb, unsigned *rate, unsigned *ch)
{
  short frame[1024 * AAC_MAX_CHANNELS];
  uint32_t ns = 0, nc = 0;
  aac_info info;

  if (is_mp4(buf, len)) {
    mp4 *m = 0;
    const mp4_track *t;
    unsigned i;
    if (mp4_open(buf, len, &m) != MP4_OK)
      return -1;
    t = mp4_audio_track(m);
    if (!t || t->codec != MP4_CODEC_AAC || !t->extradata) {
      mp4_close(m);
      return -1;
    }
    aac_decoder *d = aac_open(t->extradata, t->extradata_len, &info);
    if (!d) {
      mp4_close(m);
      return -1;
    }
    *rate = info.sample_rate;
    *ch = info.channels;
    for (i = 0; i < t->n_samples; i++) {
      unsigned sz = 0;
      const unsigned char *sd = mp4_sample_data(m, t, i, &sz);
      if (!sd || sz == 0)
        continue;
      if (aac_decode(d, sd, sz, frame, &ns, &nc) != AAC_OK)
        continue;
      if (pcm_append(pb, frame, ns * nc)) {
        aac_close(d);
        mp4_close(m);
        return -1;
      }
      *ch = nc;
    }
    aac_close(d);
    mp4_close(m);
    return 0;
  }

  if (is_adts(buf, len)) {
    aac_decoder *d = aac_open_adts();
    uint32_t off = 0;
    int got = 0;
    if (!d)
      return -1;
    while (off + 7 <= len) {
      uint32_t consumed = 0;
      if (aac_decode_adts(d, buf + off, len - off, &consumed, frame, &ns, &nc) != AAC_OK)
        break;
      if (consumed == 0)
        break;
      off += consumed;
      if (!got) {
        aac_get_info(d, &info);
        *rate = info.sample_rate;
        got = 1;
      }
      *ch = nc;
      if (pcm_append(pb, frame, ns * nc)) {
        aac_close(d);
        return -1;
      }
    }
    aac_close(d);
    return got ? 0 : -1;
  }
  return -1;
}

static int cmd_info(const char *path)
{
  unsigned len = 0, rate = 0, ch = 0;
  unsigned char *buf = slurp(path, &len);
  if (!buf) {
    printf("aac: cannot read %s\n", path);
    return 1;
  }

  if (is_mp4(buf, len)) {
    mp4 *m = 0;
    const mp4_track *t;
    aac_info info;
    aac_decoder *d;
    if (mp4_open(buf, len, &m) != MP4_OK) {
      printf("aac: not a valid MP4\n");
      free(buf);
      return 1;
    }
    t = mp4_audio_track(m);
    if (!t || t->codec != MP4_CODEC_AAC) {
      printf("aac: no AAC audio track\n");
      mp4_close(m);
      free(buf);
      return 1;
    }
    d = aac_open(t->extradata, t->extradata_len, &info);
    printf("container : MP4\n");
    printf("codec     : AAC-LC%s\n", d && info.sbr_present ? " + SBR (base rate)" : "");
    printf("samplerate: %u Hz\n", d ? info.sample_rate : t->samplerate);
    printf("channels  : %u\n", d ? info.channels : t->channels);
    printf("frames    : %u access units\n", t->n_samples);
    if (t->timescale)
      printf("duration  : %u ms\n", (unsigned)((unsigned long long)t->duration * 1000ull / t->timescale));
    if (d)
      aac_close(d);
    mp4_close(m);
  } else if (is_adts(buf, len)) {
    aac_decoder *d = aac_open_adts();
    short frame[1024 * AAC_MAX_CHANNELS];
    uint32_t consumed = 0, ns = 0, nc = 0;
    aac_info info;
    int rc = d ? aac_decode_adts(d, buf, len, &consumed, frame, &ns, &nc) : -1;
    if (rc != AAC_OK) {
      printf("aac: not a decodable ADTS stream (%d)\n", rc);
      if (d)
        aac_close(d);
      free(buf);
      return 1;
    }
    aac_get_info(d, &info);
    printf("container : ADTS (raw)\n");
    printf("codec     : AAC-LC\n");
    printf("samplerate: %u Hz\n", info.sample_rate);
    printf("channels  : %u\n", nc);
    aac_close(d);
  } else {
    printf("aac: not an AAC file (need ADTS .aac or MP4/.m4a)\n");
    free(buf);
    return 1;
  }
  (void)rate;
  (void)ch;
  free(buf);
  return 0;
}

static int cmd_decode(const char *in, const char *out)
{
  unsigned len = 0, rate = 0, ch = 0;
  unsigned char *buf = slurp(in, &len);
  struct pcmbuf pb = {0, 0, 0};
  unsigned char hdr[44];
  unsigned data_len;

  if (!buf) {
    printf("aac: cannot read %s\n", in);
    return 1;
  }
  if (decode_all(buf, len, &pb, &rate, &ch) != 0 || pb.len == 0 || ch == 0) {
    printf("aac: decode failed\n");
    free(buf);
    free(pb.data);
    return 1;
  }
  free(buf);

  data_len = pb.len * sizeof(short);
  memcpy(hdr, "RIFF", 4);
  put_le32(hdr + 4, 36 + data_len);
  memcpy(hdr + 8, "WAVE", 4);
  memcpy(hdr + 12, "fmt ", 4);
  put_le32(hdr + 16, 16);
  put_le16(hdr + 20, 1);
  put_le16(hdr + 22, ch);
  put_le32(hdr + 24, rate);
  put_le32(hdr + 28, rate * ch * 2);
  put_le16(hdr + 32, ch * 2);
  put_le16(hdr + 34, 16);
  memcpy(hdr + 36, "data", 4);
  put_le32(hdr + 40, data_len);

  if (stream_begin(out) != 0) {
    printf("aac: cannot write %s\n", out);
    free(pb.data);
    return 1;
  }
  if (stream_write(hdr, 44) != 0 || stream_write(pb.data, (int)data_len) != 0) {
    stream_end();
    printf("aac: write error\n");
    free(pb.data);
    return 1;
  }
  stream_end();
  printf("wrote %s: %u samples, %u ch @ %u Hz\n", out, pb.len / ch, ch, rate);
  free(pb.data);
  return 0;
}

int main(int argc, char **argv)
{
  if (argc >= 3 && streq(argv[1], "info"))
    return cmd_info(argv[2]);
  if (argc >= 4 && streq(argv[1], "decode"))
    return cmd_decode(argv[2], argv[3]);
  printf("usage: aac info <file.aac|.m4a>\n");
  printf("       aac decode <in.aac|.m4a> <out.wav>\n");
  return 2;
}
