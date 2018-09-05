#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#define NAPI_EXPERIMENTAL
#include <node_api.h>

#define REPORT_EVERY 1000

// An item that will be generated from the thread, passed into JavaScript, and
// ultimately marked as resolved when the JavaScript passes it back into the
// addon instance with a return value.
typedef struct ThreadItem {
  // This field is read-only once set, so it need not be protected by the mutex.
  int the_prime;

  // This field is only accessed from the secondary thread, so it also need not
  // be protected by the mutex.
  struct ThreadItem* next;

  // These two values must be protected by the mutex.
  bool call_has_returned;
  bool return_value;
} ThreadItem;

// The data associated with an instance of the addon. This takes the place of
// global static variables, while allowing multiple instances of the addon to
// co-exist.
typedef struct {
  uv_mutex_t check_status_mutex;
  uv_thread_t the_thread;
  napi_threadsafe_function tsfn;
  napi_ref thread_item_constructor;
  bool js_accepts;
} AddonData;

// This function is responsible for converting the native data coming in from
// the secondary thread to JavaScript values, and for calling the JavaScript
// function. It will also be called with `env` and `js_cb` set to NULL when
// Node.js is terminating and there are items coming in from the secondary
// thread left to process. In that case, this function does nothing, since it is
// the secondary thread that frees the items.
static void CallJs(napi_env env, napi_value js_cb, void* context, void* data) {
  AddonData* addon_data = (AddonData*)context;
  napi_value constructor;
  if (addon_data->js_accepts && !(env == NULL || js_cb == NULL)) {
    napi_value undefined, argv[2];
    // Retrieve the JavaScript `undefined` value. This will serve as the `this`
    // value for the function call.
    assert(napi_get_undefined(env, &undefined) == napi_ok);

    // Retrieve the constructor for the JavaScript class from which the item
    // holding the native data will be constructed.
    assert(napi_get_reference_value(env,
                                    addon_data->thread_item_constructor,
                                    &constructor) == napi_ok);

    // Construct a new instance of the JavaScript class to hold the native item.
    assert(napi_new_instance(env, constructor, 0, NULL, &argv[0]) == napi_ok);

    // Associate the native item with the JavaScript object. We assume that the
    // JavaScript side will eventually pass this JavaScript object back to us
    // via RegisterReturnValue, which will allow the eventual deallocation of
    // the native data. That's why we do not provide a finalizer here.
    assert(napi_wrap(env, argv[0], data, NULL, NULL, NULL) == napi_ok);

    // Convert the prime number to a number `napi_value` we can pass into
    // JavaScript.
    assert(napi_create_int32(env,
                             ((ThreadItem*)data)->the_prime,
                             &argv[1]) == napi_ok);

    // Call the JavaScript function with the item as wrapped into an instance of
    // the JavaScript `ThreadItem` class and the prime.
    assert(napi_call_function(env, undefined, js_cb, 2, argv, NULL) == napi_ok);
  }
}

// When the thread is finished we join it to prevent memory leaks. We can safely
// set `addon_data->tsfn` to NULL, because the thread-safe function will be
// cleaned up in the background in response to the secondary thread having
// called `napi_release_threadsafe_function()`.
static void ThreadFinished(napi_env env, void* data, void* context) {
  (void) context;
  AddonData* addon_data = (AddonData*)data;
  assert(uv_thread_join(&(addon_data->the_thread)) == 0);
  addon_data->tsfn = NULL;
}

// The secondary thread produces prime numbers using a very inefficient
// algorithm and calls into JavaScript with every REPORT_EVERYth prime number.
// After each call it checks whether any of the previous calls have produced a
// return value, and, if so, whether that return value is false. A false return
// value indicates that the JavaScript side is no longer interested in receiving
// any values. On the JavaScript thread, this is marked in
// `addon_data->js_accepts`. When set to false, the JavaScript thread will not
// access the thread items any further, so they can be safely deleted on this
// thread.
static void PrimeThread(void* data) {
  AddonData* addon_data = (AddonData*) data;
  int idx_outer, idx_inner;
  int prime_count = 0;
  ThreadItem* first = NULL;
  ThreadItem* current = NULL;
  ThreadItem* previous = NULL;
  ThreadItem* returned = NULL;

  // Check each integer whether it's a prime.
  for (idx_outer = 2 ;; idx_outer++) {

    // Check whether `idx_outer` is divisible by anything up to and not
    // including itself.
    for (idx_inner = 2;
        idx_inner < idx_outer && idx_outer % idx_inner != 0;
        idx_inner++);

    // If we find a prime, and it is REPORT_EVERY primes away from the previous
    // prime we found, then we send it to JavaScript.
    if (idx_inner >= idx_outer && (++prime_count % REPORT_EVERY) == 0) {
      // Create a new thread item and attach it to the list of items.
      current = memset(malloc(sizeof(*current)), 0, sizeof(*current));
      current->the_prime = idx_outer;
      current->call_has_returned = false;
      current->return_value = false;
      current->next = first;
      first = current;

      // Pass the new item into JavaScript.
      assert(napi_call_threadsafe_function(addon_data->tsfn,
                                           first,
                                           napi_tsfn_blocking) == napi_ok);
    }

    // Pass over all outstanding thread items and check whether any of them have
    // returned.
    for (current = first, previous = NULL, returned = NULL;
        current != NULL && returned == NULL;
        previous = current,
        current = current->next) {
      uv_mutex_lock(&(addon_data->check_status_mutex));
      if (current->call_has_returned) {
        // Unhook the call that has returned from the list.
        if (previous != NULL) {
          previous->next = current->next;
        } else {
          first = current->next;
        }
        returned = current;
      }
      uv_mutex_unlock(&(addon_data->check_status_mutex));
    }

    // If a call has returned true, we free the data associated with it.
    // Otherwise we terminate the thread by breaking out of the loop.
    if (returned != NULL) {
      if (returned->return_value) {
        free(returned);
      } else {
        break;
      }
    }
  }

  // Before terminating the thread we free the remaining queue items. CallJs
  // will be called with pointers to these items, perhaps after this thread has
  // already freed them, but that's OK, because addon_data->js_accepts will
  // certainly have been set to false, and so CallJs will not dereference the
  // stale pointers.
  for (current = first; current != NULL;) {
    previous = current;
    current = current->next;
    free(previous);
  }

  // Release the thread-safe function. This causes it to be cleaned up in the
  // background.
  assert(napi_release_threadsafe_function(addon_data->tsfn,
                                          napi_tsfn_release) == napi_ok);
}

// Binding that can be called from JavaScript to start the asynchronous prime
// generator.
static napi_value StartThread(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value js_cb, work_name;
  AddonData* addon_data;

  // The function accepts one parameter - the JavaScript callback function to
  // call.
  assert(napi_get_cb_info(env,
                          info,
                          &argc,
                          &js_cb,
                          NULL,
                          (void*)&addon_data) == napi_ok);

  // We do not create a second thread if one is already running.
  assert(addon_data->tsfn == NULL && "Work already in progress");

  addon_data->js_accepts = true;

  // This string describes the asynchronous work.
  assert(napi_create_string_utf8(env,
                                 "Thread-safe Function Round Trip Example",
                                 NAPI_AUTO_LENGTH,
                                 &work_name) == napi_ok);

  // The thread-safe function will be created with an unlimited queue and with
  // an initial reference of thread count of 1. The secondary thread will
  // release the thread-safe function, decreasing its thread count to 0, thereby
  // setting off the process of cleaning up the thread-safe function.
  assert(napi_create_threadsafe_function(env,
                                         js_cb,
                                         NULL,
                                         work_name,
                                         0,
                                         1,
                                         addon_data,
                                         ThreadFinished,
                                         addon_data,
                                         CallJs,
                                         &addon_data->tsfn) == napi_ok);

  // Create the thread that will produce primes.
  assert(uv_thread_create(&(addon_data->the_thread), PrimeThread, addon_data) == 0);

  return NULL;
}

// We use a separate binding to register a return value for a given call into
// JavaScript. This allows the JavaScript side to asynchronously determine the
// return value.
static napi_value RegisterReturnValue(napi_env env, napi_callback_info info) {
  // This function accepts two parameters:
  // 1. The thread item passed into JavaScript via CallJs
  // 2. The desired return value.
  size_t argc = 2;
  napi_value argv[2];
  napi_value constructor;
  AddonData* addon_data;
  bool right_instance, return_value;
  ThreadItem* item;

  // Retrieve the parameters with which this function was called.
  assert(napi_get_cb_info(env,
                          info,
                          &argc,
                          argv,
                          NULL,
                          (void*)&addon_data) == napi_ok);

  assert(argc == 2 && "Exactly two arguments were received");

  // Retrieve the constructor for `ThreadItem` instances.
  assert(napi_get_reference_value(env,
                                  addon_data->thread_item_constructor,
                                  &constructor) == napi_ok);

  // Make sure the first parameter is an instance of the `ThreadItem` class.
  assert(napi_instanceof(env,
                         argv[0],
                         constructor,
                         &right_instance) == napi_ok);
  assert(right_instance && "First argument is a `ThreadItem`");

  // Retrieve the native data from the item.
  assert(napi_unwrap(env, argv[0], (void**)&item) == napi_ok);

  // Retrieve the desired return value.
  assert(napi_get_value_bool(env, argv[1], &return_value) == napi_ok);

  // Set `js_accepts` to false in case the JavaScript callback returned false.
  if (addon_data->js_accepts) {
    addon_data->js_accepts = return_value;
  }

  // Mark the thread item as resolved, and record the JavaScript return value.
  uv_mutex_lock(&(addon_data->check_status_mutex));
  item->call_has_returned = true;
  item->return_value = return_value;
  uv_mutex_unlock(&(addon_data->check_status_mutex));

  return NULL;
}

// Constructor for instances of the ThreadItem class. This doesn't need to do
// anything since all we want the class for is to be able to type-tag objects
// that carry within them pointers to native ThreadItem structures.
static napi_value ThreadItemConstructor(napi_env env, napi_callback_info info) {
  return NULL;
}

static void addon_is_unloading(napi_env env, void* data, void* hint) {
  AddonData* addon_data = (AddonData*)data;
  uv_mutex_destroy(&(addon_data->check_status_mutex));
  assert(napi_delete_reference(env,
                               addon_data->thread_item_constructor) == napi_ok);
  free(data);
}

// Initialize an instance of this addon. This function may be called multiple
// times if Node.js is running on multiple threads, or if there are multiple
// contexts.
/*napi_value*/ NAPI_MODULE_INIT(/*napi_env env, napi_value exports*/) {
  // Create the native data that will be associated with this instance of the
  // addon.
  AddonData* addon_data =
      memset(malloc(sizeof(*addon_data)), 0, sizeof(*addon_data));

  // Attach the addon data to the exports object to ensure that they are
  // destroyed together.
  assert(napi_wrap(env,
                   exports,
                   addon_data,
                   addon_is_unloading,
                   NULL,
                   NULL) == napi_ok);

  // Initialize the various members of the AddonData associated with this addon
  // instance.
  assert(uv_mutex_init(&(addon_data->check_status_mutex)) == 0);

  napi_value thread_item_class;

  assert(napi_define_class(env,
                           "ThreadItem",
                           NAPI_AUTO_LENGTH,
                           ThreadItemConstructor,
                           addon_data,
                           0,
                           NULL,
                           &thread_item_class) == napi_ok);
  assert(napi_create_reference(env,
                               thread_item_class,
                               1,
                               &(addon_data->thread_item_constructor)) ==
                                  napi_ok);

  // Expose the two bindings this addon provides.
  napi_property_descriptor export_properties[] = {
    {
      "startThread",
      NULL,
      StartThread,
      NULL,
      NULL,
      NULL,
      napi_default,
      addon_data
    },
    {
      "registerReturnValue",
      NULL,
      RegisterReturnValue,
      NULL,
      NULL,
      NULL,
      napi_default,
      addon_data
    }
  };

  assert(napi_define_properties(env,
                                exports,
                                sizeof(export_properties) /
                                    sizeof(export_properties[0]),
                                export_properties) == napi_ok);

  return exports;
}
