#include <errno.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/check/gstcheck.h>

#include "../gst/rockchipmpp/gstmppallocator.h"

extern void mpp_mock_reset(void);
extern void mpp_mock_fail_next_buffer_import(void);
extern unsigned mpp_mock_buffer_import_calls(void);
extern unsigned mpp_mock_buffer_inc_ref_calls(void);
extern unsigned mpp_mock_buffer_ref_count(MppBuffer buffer);
extern int __real_dup(int fd);
extern GstMemory *__real_gst_fd_allocator_alloc(GstAllocator *allocator, gint fd,
                                                 gsize size,
                                                 GstFdMemoryFlags flags);

static gboolean fail_next_dup;
static guint fd_allocator_calls;
static gint last_fd_allocator_fd;

int __wrap_dup(int fd) {
  if (fail_next_dup) {
    fail_next_dup = FALSE;
    errno = EMFILE;
    return -1;
  }

  return __real_dup(fd);
}

GstMemory *__wrap_gst_fd_allocator_alloc(GstAllocator *allocator, gint fd,
                                         gsize size, GstFdMemoryFlags flags) {
  fd_allocator_calls++;
  last_fd_allocator_fd = fd;
  return __real_gst_fd_allocator_alloc(allocator, fd, size, flags);
}

static MppBuffer allocate_buffer(GstAllocator *allocator) {
  MppBuffer buffer = gst_mpp_allocator_alloc_mppbuf(allocator, 4096);
  fail_unless(buffer != NULL);
  fail_unless(mpp_buffer_get_fd(buffer) >= 0);
  fail_unless_equals_int(mpp_mock_buffer_ref_count(buffer), 1);
  return buffer;
}

GST_START_TEST(test_failed_external_import_does_not_take_a_buffer_reference) {
  mpp_mock_reset();
  GstAllocator *source = gst_mpp_allocator_new();
  GstAllocator *target = gst_mpp_allocator_new();
  fail_unless(source != NULL && target != NULL);

  MppBuffer buffer = allocate_buffer(source);
  fail_unless(gst_mpp_allocator_get_index(source) !=
              gst_mpp_allocator_get_index(target));
  mpp_mock_fail_next_buffer_import();

  GstMemory *memory = gst_mpp_allocator_import_mppbuf(target, buffer);
  fail_unless(memory == NULL);
  fail_unless_equals_int(mpp_mock_buffer_import_calls(), 1);
  fail_unless_equals_int(mpp_mock_buffer_inc_ref_calls(), 0);
  fail_unless_equals_int(mpp_mock_buffer_ref_count(buffer), 1);

  mpp_buffer_put(buffer);
  gst_object_unref(source);
  gst_object_unref(target);
}
GST_END_TEST

GST_START_TEST(test_failed_same_group_dup_does_not_take_a_buffer_reference) {
  mpp_mock_reset();
  fd_allocator_calls = 0;
  last_fd_allocator_fd = -2;
  GstAllocator *allocator = gst_mpp_allocator_new();
  fail_unless(allocator != NULL);

  MppBuffer buffer = allocate_buffer(allocator);
  fail_next_dup = TRUE;

  GstMemory *memory = gst_mpp_allocator_import_mppbuf(allocator, buffer);
  fail_unless(memory == NULL);
  fail_unless_equals_int(fd_allocator_calls, 0);
  fail_unless_equals_int(last_fd_allocator_fd, -2);
  fail_unless_equals_int(mpp_mock_buffer_import_calls(), 0);
  fail_unless_equals_int(mpp_mock_buffer_inc_ref_calls(), 0);
  fail_unless_equals_int(mpp_mock_buffer_ref_count(buffer), 1);

  mpp_buffer_put(buffer);
  gst_object_unref(allocator);
}
GST_END_TEST

static Suite *mpp_allocator_seam_suite(void) {
  Suite *suite = suite_create("mpp_allocator_seam");
  TCase *test_case = tcase_create("allocator");
  tcase_add_test(test_case,
                 test_failed_external_import_does_not_take_a_buffer_reference);
  tcase_add_test(test_case,
                 test_failed_same_group_dup_does_not_take_a_buffer_reference);
  suite_add_tcase(suite, test_case);
  return suite;
}

GST_CHECK_MAIN(mpp_allocator_seam)
