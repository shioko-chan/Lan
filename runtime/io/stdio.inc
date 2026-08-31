/* Standard IO operations share the runtime's Text/List allocation and ARC.
 * Constructors do not call these functions; generated action callbacks do. */
#include <signal.h>

enum { LAN_IO_TEXT_LIMIT = 8 * 1024 * 1024, LAN_IO_ARGUMENT_LIMIT = 65536 };
static int32_t io_argc;
static char** io_argv;

extern "C" void lan_io_initialize(int32_t argc, void* argv) {
  io_argc = argc;
  io_argv = static_cast<char**>(argv);
}

static int32_t io_error(char* error, uint64_t capacity, const char* action,
                         const char* detail, int system_error) {
  if (system_error)
    snprintf(error, capacity, "%s: %s (errno=%d: %s)", action, detail,
             system_error, strerror(system_error));
  else snprintf(error, capacity, "%s: %s", action, detail);
  return -1;
}

static int io_valid_text(const char* bytes, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    utf8proc_int32_t codepoint;
    utf8proc_ssize_t size = utf8proc_iterate(
        (const utf8proc_uint8_t*)bytes + offset, length - offset, &codepoint);
    // Text currently uses a NUL-terminated representation.
    if (size <= 0 || codepoint == 0) return 0;
    offset += (size_t)size;
  }
  return 1;
}

extern "C" int32_t lan_io_read_stdin(LanValue* output, char* error, uint64_t capacity) {
  LanTextBuilder buffer;
  for (;;) {
    char chunk[4096];
    size_t request = LAN_IO_TEXT_LIMIT - buffer.length + 1;
    if (request > sizeof(chunk)) request = sizeof(chunk);
    errno = 0;
    size_t count = fread(chunk, 1, request, stdin);
    if (ferror(stdin)) {
      int saved = errno ? errno : EIO;
      return io_error(error, capacity, "read_stdin", "读取标准输入失败", saved);
    }
    if (count > LAN_IO_TEXT_LIMIT - buffer.length) {
      return io_error(error, capacity, "read_stdin", "标准输入超过 8 MiB 上限", 0);
    }
    if (count) text_builder_append_bytes(&buffer, chunk, count);
    if (feof(stdin)) break;
  }
  if (!io_valid_text(buffer.data, buffer.length)) {
    return io_error(error, capacity, "read_stdin", "输入必须是有效 UTF-8，且不能包含 NUL", 0);
  }
  char* text = lan_text_from_cstr(buffer.data ? buffer.data : "");
  *output = LanValue{LAN_TEXT, 0, text};
  return 0;
}

extern "C" int32_t lan_io_write_stdout(const char* text, LanValue* output,
                            char* error, uint64_t capacity) {
  size_t length = strlen(text);
  if (!io_valid_text(text, length))
    return io_error(error, capacity, "write_stdout", "输出必须是有效 UTF-8", 0);
  // A closed pipe must report an IO error, not terminate via SIGPIPE.
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    return io_error(error, capacity, "write_stdout", "无法配置 SIGPIPE", errno);
  errno = 0;
  size_t written = fwrite(text, 1, length, stdout);
  int saved = errno;
  if (written != length || ferror(stdout))
    return io_error(error, capacity, "write_stdout", "写入标准输出失败", saved ? saved : EIO);
  if (fflush(stdout) != 0)
    return io_error(error, capacity, "write_stdout", "刷新标准输出失败", errno ? errno : EIO);
  *output = LanValue{LAN_UNIT, 0, NULL};
  return 0;
}

extern "C" int32_t lan_io_arguments(LanValue* output, char* error, uint64_t capacity) {
  uint64_t count = io_argc > 1 ? (uint64_t)(io_argc - 1) : 0;
  if (count > LAN_IO_ARGUMENT_LIMIT)
    return io_error(error, capacity, "arguments", "参数数量超过 65536 上限", 0);
  size_t total = 0;
  for (uint64_t i = 0; i < count; ++i) {
    size_t length = strlen(io_argv[i + 1]);
    if (length > LAN_IO_TEXT_LIMIT - total)
      return io_error(error, capacity, "arguments", "参数总长度超过 8 MiB 上限", 0);
    total += length;
    if (!io_valid_text(io_argv[i + 1], length))
      return io_error(error, capacity, "arguments", "参数必须是有效 UTF-8", 0);
  }
  LanList* result = make_typed_list(count, LAN_TEXT);
  auto resultOwner = OwnedValue::adopt({LAN_LIST, 0, result});
  for (uint64_t i = 0; i < count; ++i) {
    store_list_owned(result, i, OwnedValue::adopt(
        {LAN_TEXT, 0, lan_text_from_cstr(io_argv[i + 1])}));
  }
  *output = resultOwner.take();
  return 0;
}
