// Example illustrating the use of a thread-safe function.

// Load the N-API addon which creates the thread-safe function and the threads.
const binding = require('bindings')('tsfn_example');

// We count how many additional threads we have fired off since the start.
let additionalThreads = 0;

// The addon returns the thread-safe function in an external. We store it and
// set this value to null when we finally release the thread-safe function.
let tsFn;

// The various threads on the native side will be calling this function.
function tsCallback(value, threadQuitting) {
  const isEven = !(value % 2);
  console.log('tsCallback: value is ' + value +
    (threadQuitting ? ", thread is quitting" : ""));

  // If the thread informs us that it has produced its last value and is
  // quitting, we spawn another thread, unless we've spawned more than two
  // additional ones, in which case we release the thread-safe function and
  // quit.
  if (threadQuitting) {
    if (additionalThreads < 2) {
      additionalThreads++;
      console.log('tsCallback: Creating another ' +
          (isEven ? 'even' : 'odd') + ' thread');
      binding.createThread(tsFn, isEven);
    } else if (!!tsFn) {
      binding.releaseFunction(tsFn);
      // After releasing the thread-safe function this callback may still be
      // called, because the threads may still be producing items, and/or there
      // may still be items in the queue for this callback to process. So, we
      // set tsFn to null to make sure we do not call releaseFunction() again.
      tsFn = null;
    }
  }
}

// This callback illustrates that the thread-safe function correctly cleans up
// after itself.
function finalizeCallback(value) {
  console.log('thread-safe function cleanup is in progress.');
}

// Create the thread-safe function, passing it the JavaScript function which the
// threads will call, and a second JavaScript function which will be called
// during the thread-safe function's cleanup.
tsFn = binding.createFunction(tsCallback, finalizeCallback);

// Create the initial set of threads.
console.log("Creating thread for even numbers.");
binding.createThread(tsFn, true);

console.log("Creating thread for odd numbers.");
binding.createThread(tsFn, false);


