'use strict';
// Support only: persistent assertion state survives a caught exception.
exports.createContext=function(group){
 let checks=0,failed=0;
 const c=Object.freeze({
  check(condition,message='content assertion'){
   checks++;
   if(condition!==true){failed++;throw new Error(`${group}: ${message}`);}
  },
  skip(){failed++;throw new Error(`${group}: skipped body`);}
 });
 return Object.freeze({c,finish(){if(failed||checks===0)throw new Error(`${group}: checks=${checks} failed=${failed}`);return checks;}});
};
