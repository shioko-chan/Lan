#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

typedef struct { int32_t tag; double number; void* pointer; } LanValue;
void* lan_io_new(void (*execute)(void*, LanValue*), uint64_t count);
void lan_io_capture(void*, uint64_t, int32_t, double, void*);
void* lan_io_capture_at(void*, uint64_t);
void lan_io_run(void*, LanValue*);
char* lan_text_from_cstr(const char*);
char* lan_text_concat(const char*, const char*);
void lan_value_release(int32_t, double, void*);
void lan_arc_assert_empty(void);
void lan_io_initialize(int32_t, void*);
int32_t lan_io_read_stdin(LanValue*, char*, uint64_t);
int32_t lan_io_write_stdout(const char*, LanValue*, char*, uint64_t);
int32_t lan_io_arguments(LanValue*, char*, uint64_t);
int32_t lan_list_lookup_at(void*, uint64_t, LanValue*);

static unsigned executions;
static void execute(void* action, LanValue* output) {
  ++executions;
  LanValue* captured = lan_io_capture_at(action, 0);
  *output = (LanValue){3, 0, lan_text_concat(captured->pointer, "!")};
}

static void forward(void* action, LanValue* output) {
  LanValue* captured = lan_io_capture_at(action, 0);
  lan_io_run(captured->pointer, output);
}

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "failed: %s\n", #test); exit(1); } } while (0)

static void check_standard_io(void) {
  char error[512];
  LanValue output, item;
  char* args[] = {"executable", "", "中文", "--flag", NULL};
  lan_io_initialize(4, args);
  CHECK(lan_io_arguments(&output, error, sizeof(error)) == 0);
  CHECK(output.tag == 4);
  CHECK(lan_list_lookup_at(output.pointer, 0, &item) && strcmp(item.pointer, "") == 0);
  CHECK(lan_list_lookup_at(output.pointer, 1, &item) && strcmp(item.pointer, "中文") == 0);
  CHECK(!lan_list_lookup_at(output.pointer, 3, &item));
  lan_value_release(output.tag, output.number, output.pointer);
  char invalid[] = {(char)0xff, 0};
  args[1] = invalid;
  CHECK(lan_io_arguments(&output, error, sizeof(error)) != 0);
  CHECK(strstr(error, "UTF-8"));
  lan_io_initialize(65538, args);
  CHECK(lan_io_arguments(&output, error, sizeof(error)) != 0);
  CHECK(strstr(error, "65536"));

  int saved = dup(STDIN_FILENO);
  CHECK(saved >= 0);
  FILE* input = tmpfile();
  CHECK(input);
  CHECK(fwrite("a\0b", 1, 3, input) == 3);
  rewind(input);
  CHECK(dup2(fileno(input), STDIN_FILENO) >= 0);
  clearerr(stdin);
  CHECK(lan_io_read_stdin(&output, error, sizeof(error)) != 0);
  CHECK(strstr(error, "NUL"));
  fclose(input);
  CHECK(close(STDIN_FILENO) == 0);
  clearerr(stdin);
  CHECK(lan_io_read_stdin(&output, error, sizeof(error)) != 0);
  CHECK(strstr(error, "errno="));
  CHECK(dup2(saved, STDIN_FILENO) >= 0);
  close(saved);
  clearerr(stdin);

  // No reader: write_stdout must return an error rather than die from SIGPIPE.
  int pipefd[2];
  CHECK(pipe(pipefd) == 0);
  close(pipefd[0]);
  pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    CHECK(dup2(pipefd[1], STDOUT_FILENO) >= 0);
    close(pipefd[1]);
    CHECK(lan_io_write_stdout("closed pipe", &output, error, sizeof(error)) != 0);
    CHECK(strstr(error, "write_stdout") && strstr(error, "errno="));
    _exit(0);
  }
  close(pipefd[1]);
  int status;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  lan_arc_assert_empty();
}

int main(void) {
  void* text = lan_text_from_cstr("captured");
  void* action = lan_io_new(execute, 1);
  lan_io_capture(action, 0, 3, 0, text);
  lan_value_release(3, 0, text);
  void* wrapper = lan_io_new(forward, 1);
  lan_io_capture(wrapper, 0, 8, 0, action);
  lan_value_release(8, 0, action);
  CHECK(executions == 0);
  LanValue first, second;
  lan_io_run(wrapper, &first);
  lan_io_run(wrapper, &second);
  CHECK(executions == 2);
  CHECK(first.tag == 3 && second.tag == 3);
  CHECK(first.pointer != second.pointer);
  CHECK(strcmp(first.pointer, "captured!") == 0);
  CHECK(strcmp(second.pointer, "captured!") == 0);
  lan_value_release(8, 0, wrapper);
  CHECK(strcmp(first.pointer, "captured!") == 0);
  lan_value_release(first.tag, first.number, first.pointer);
  lan_value_release(second.tag, second.number, second.pointer);
  lan_arc_assert_empty();
  check_standard_io();
  return 0;
}
