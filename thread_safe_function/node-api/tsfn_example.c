#include <stdlib.h>
#include <uv.h>
#define NAPI_EXPERIMENTAL
#include <node_api.h>
#include <assert.h>

// The number of iterations a thread will perform is a tunable parameter.
#define ITERATION_COUNT 100

// This structure holds per-thread state.
typedef struct {
  uv_thread_t thread;
  napi_threadsafe_function ts_fn;
  bool is_even;
} ThreadData;

// This is the structure of an item we wish to send to JavaScript.
typedef struct {
  int value;
  ThreadData* thread_to_join;
} JSData;

// This context governs the operation of the thread-safe function from beginning
// to end.
typedef struct {
  bool main_released;
  napi_threadsafe_function ts_fn;
  napi_ref js_finalize_cb;
} Context;

// This is the worker thread. It produces even or odd numbers, depending on the
// contents of the ThreadData it receives.
static void one_thread(void* data) {
  napi_status status;
  ThreadData* thread_data = data;

  // The first step is always to acquire the thread-safe function. This
  // indicates that the function must not be destroyed, because it's still in
  // use.
  status = napi_acquire_threadsafe_function(thread_data->ts_fn);
  assert(status == napi_ok);

  // Perform our iterations, and call the thread-safe function with each value.
  for (int n = 0; n < ITERATION_COUNT ; n++) {
    JSData* item = malloc(sizeof(*item));

    item->value = n * 2 + (thread_data->is_even ? 0 : 1);

    item->thread_to_join = ((n == ITERATION_COUNT - 1) ? thread_data : NULL);
    status = napi_call_threadsafe_function(thread_data->ts_fn,
                                           item,
                                           napi_tsfn_blocking);

    // A return value of napi_closing informs us that the thread-safe function
    // is about to be destroyed. Therefore this thread must exit immediately,
    // without making any further thread-safe-function-related calls.
    if (status == napi_closing) {
      return;
    }
  }

  // The final task of this thread is to release the thread-safe function. This
  // indicates that, if there are no other threads using of the function, it may
  // be destroyed.
  status =
      napi_release_threadsafe_function(thread_data->ts_fn, napi_tsfn_release);
  assert(status == napi_ok);
}

// This function is responsible for translating the JSData items produced by the
// secondary threads into an array of napi_value JavaScript values which may be
// passed into a call to the JavaScript function.
static void call_into_javascript(napi_env env,
                                 napi_value js_callback,
                                 void* ctx,
                                 void* data) {
  JSData* item = data;
  napi_status status;
  napi_value args[2], undefined;

  // env and js_callback may be NULL if we're in cleanup mode.
  if (!(env == NULL || js_callback == NULL)) {
    // The first parameter to the JavaScript callback will be the integer value
    // that was produced on the thread.
    status = napi_create_int32(env, item->value, &args[0]);
    assert(status == napi_ok);

    // The second value will be a boolean indicating whether the thread that
    // produced the value is done.
    status = napi_get_boolean(env, !!(item->thread_to_join), &args[1]);
    assert(status == napi_ok);

    // Since a function call must have a receiver, we use undefined, as in
    // strict mode.
    status = napi_get_undefined(env, &undefined);
    assert(status == napi_ok);

    // Call into JavaScript.
    status = napi_call_function(env, undefined, js_callback, 2, args, NULL);
    assert(status == napi_ok || status == napi_pending_exception);
  }

  // If the thread producing this item has indicated that its job is finished
  // by setting thread_to_join to something other than NULL, we must call
  // uv_thread_join() to avoid a resource leak and we must free all the data
  // associated with the thread.
  if (item->thread_to_join) {
    uv_thread_join(&(item->thread_to_join->thread));
    free(item->thread_to_join);
  }

  // Free the data item created by the thread.
  free(item);
}

// When all threads have released the thread-safe function, we may free any
// resources associated with it. Additionally, we call one last time into
// JavaScript to inform that the thread-safe function is about to be destroyed.
static void finalize_tsfn(napi_env env, void* data, void* ctx) {
  napi_status status;
  Context* context = ctx;

  // Retrieve the JavaScript undefined value so it may serve as a receiver
  // for the JavaScript callback.
  napi_value undefined;
  status = napi_get_undefined(env, &undefined);
  assert(status == napi_ok);

  // Retrieve the JavaScript finalize callback from the persistent reference.
  napi_value js_finalize_cb;
  status = napi_get_reference_value(env,
                                    context->js_finalize_cb,
                                    &js_finalize_cb);
  assert(status == napi_ok);

  // Call the JavaScript finalizer.
  status = napi_call_function(env, undefined, js_finalize_cb, 0, NULL, NULL);
  assert(status == napi_ok || status == napi_pending_exception);

  // Delete the persistent reference to the JavaScript finalizer callback.
  status = napi_delete_reference(env, context->js_finalize_cb);
  assert(status == napi_ok);

  // Finally, we free the context associated with this thread-safe function.
  free(context);
}

// Signature in JavaScript: createFunction(callback, finalizer)
// Creates a thread-safe function and returns it wrapped in an external.
static napi_value CreateFunction(napi_env env, napi_callback_info info) {
  napi_status status;

  // Ensure that we have two arguments.
  size_t argc = 2;
  napi_value args[2];
  status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
  assert(argc == 2 && status == napi_ok);

  // Ensure that the first argument is a JavaScript function.
  napi_valuetype type_of_argument;
  status = napi_typeof(env, args[0], &type_of_argument);
  assert(status == napi_ok && type_of_argument == napi_function);

  // Ensure that the second argument is a JavaScript function.
  status = napi_typeof(env, args[1], &type_of_argument);
  assert(status == napi_ok && type_of_argument == napi_function);

  // Create a string that describes this asynchronous operation.
  napi_value async_name;
  status = napi_create_string_utf8(env,
                                   "Even/Odd Producer",
                                   NAPI_AUTO_LENGTH,
                                   &async_name);
  assert(status == napi_ok);

  // Allocate and initialize a context that will govern this thread-safe
  // function. This includes creating the thread-safe function itself.
  Context* context = malloc(sizeof(*context));
  assert(context != NULL);
  context->main_released = false;
  status = napi_create_reference(env, args[1], 1, &(context->js_finalize_cb));
  assert(status == napi_ok);
  status = napi_create_threadsafe_function(env,
                                          args[0],
                                          NULL, // optional async object
                                          async_name, // name of async operation
                                          20, // max queue size
                                          1, // initial thread count
                                          NULL, // thread finalize data
                                          finalize_tsfn, // finalizer
                                          context, // context,
                                          call_into_javascript, // marshaller
                                          &context->ts_fn);
  assert(status == napi_ok);

  // Wrap the thread-safe function into a JavaScript external value so we may
  // pass it around in JavaScript. The thread-safe function has its own cleanup,
  // so we do not need to create the external value with a finalizer.
  napi_value external;
  status = napi_create_external(env, context->ts_fn, NULL, NULL, &external);
  assert(status == napi_ok);

  return external;
}

// Signature in JavaScript: CreateThread(tsfn, even)
// Adds a thread to an existing thread-safe function.
static napi_value CreateThread(napi_env env, napi_callback_info info) {
  napi_status status;

  // Ensure that we have been passed two arguments.
  size_t argc = 2;
  napi_value args[2];
  status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
  assert(status == napi_ok);

  napi_valuetype value_type;

  // Ensure that the first argument is an external. Note that this is not
  // a very strong indication that the external we receive here is the same
  // as the external we returned from CreateFunction() above. In production
  // you may wish to ascertain type correctness by using JavaScript classes,
  // instances, napi_wrap(), and napi_instanceof(). Employing these tools is
  // beyond the scope of this example.
  status = napi_typeof(env, args[0], &value_type);
  assert(status == napi_ok && value_type == napi_external);

  // Ensure that the second argument is a boolean to indicate whether the new
  // thread is to produce even or odd numbers.
  status = napi_typeof(env, args[1], &value_type);
  assert(status == napi_ok && value_type == napi_boolean);

  // Retrieve the thread-safe function from the external.
  napi_threadsafe_function ts_fn;
  void* data;
  status = napi_get_value_external(env, args[0], &data);
  assert(status == napi_ok);
  ts_fn = data;

  // Retrieve the boolean from the second argument.
  bool is_even;
  status = napi_get_value_bool(env, args[1], &is_even);
  assert(status == napi_ok);

  // Allocate data for a new thread.
  ThreadData* thread_data = malloc(sizeof(*thread_data));
  assert(thread_data != NULL);
  thread_data->ts_fn = ts_fn;
  thread_data->is_even = is_even;

  // Start the new thread.
  int result;
  result = uv_thread_create(&(thread_data->thread), one_thread, thread_data);
  assert(result == 0);

  return NULL;
}

// When the logic in JavaScript decides that no more new threads will be created
// it calls this function to release the thread-safe function from the main
// thread. Note that this does not mean that there will be no further calls into
// JavaScript. The threads will all place their values into the queue, and, for
// each value, a call into JavaScript will still be made. The thread-safe
// function will be destroyed when the queue becomes empty.
static napi_value ReleaseFunction(napi_env env, napi_callback_info info) {
  napi_status status;

  // Retrieve the arguments with which this binding was called.
  size_t argc = 1;
  napi_value external;
  status = napi_get_cb_info(env, info, &argc, &external, NULL, NULL);
  assert(status == napi_ok);

  // Assert that an external was received. See above discussion regarding weak
  // type guarantees.
  napi_valuetype value_type;
  status = napi_typeof(env, external, &value_type);
  assert(status == napi_ok && value_type == napi_external);

  // Retrieve the thread-safe function from the external.
  void* data;
  status = napi_get_value_external(env, external, &data);
  assert(status == napi_ok);
  napi_threadsafe_function ts_fn = data;

  // Release the thread-safe function on behalf of the main thread.
  status = napi_release_threadsafe_function(ts_fn, napi_tsfn_release);
  assert(status == napi_ok);

  return NULL;
}

// Module initialization.
static napi_value Init(napi_env env, napi_value exports) {
  // Declare the above three bindings.
  napi_property_descriptor props[] = {
    { "createFunction", NULL, CreateFunction, NULL, NULL, NULL, napi_enumerable, NULL },
    { "createThread", NULL, CreateThread, NULL, NULL, NULL, napi_enumerable, NULL },
    { "releaseFunction", NULL, ReleaseFunction, NULL, NULL, NULL, napi_enumerable, NULL },
  };

  // Attach them to the exports object.
  napi_status status = napi_define_properties(env,
                                              exports,
                                              sizeof(props) / sizeof(*props), props);
  assert(status == napi_ok);

  // Return the newly adorned exports object.
  return exports;
}

// Mark this as a N-API module.
NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
