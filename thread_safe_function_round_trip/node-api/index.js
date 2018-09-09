const bindings = require('bindings')('round_trip');
var ok = true;

bindings.startThread((item, thePrime) => {
  console.log('The prime: ' + thePrime);

  // Answer the call with a 90% probability of returning true somewhere between
  // 200 and 400 ms from now. To prevent segmentation fault (see the output below),
  // make sure we return false only once.
  setTimeout(() => {
    const theAnswer = ok ? (Math.random() > 0.1) : true;
    if (ok) ok = theAnswer;
    console.log(thePrime + ': answering with ' + theAnswer);
    bindings.registerReturnValue(item, theAnswer);
  }, Math.random() * 200 + 200);
});
/*
The prime: 7919
The prime: 17389
The prime: 27449
7919: answering with false
17389: answering with true
27449: answering with false
Segmentation fault: 11
*/ 
