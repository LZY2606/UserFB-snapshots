import Settings from '../src/core/Settings.js';
import EventBus from '../src/core/EventBus.js';
import ThroughputController from '../src/streaming/controllers/ThroughputController.js';
import MediaPlayerEvents from '../src/streaming/MediaPlayerEvents.js';
import MetricsConstants from '../src/streaming/constants/MetricsConstants.js';
import Constants from '../src/streaming/constants/Constants.js';
export {Constants, Settings};
export function setup() {
 const context={};const settings=Settings(context).getInstance();
 settings.update({streaming:{abr:{throughput:{averageCalculationMode:Constants.THROUGHPUT_CALCULATION_MODES.ARITHMETIC_MEAN,useResourceTimingApi:false,useNetworkInformationApi:{fetch:false,xhr:false},sampleSettings:{enableSampleSizeAdjustment:false,maxMeasurementsToKeep:4}}}}});
 const controller=ThroughputController(context).getInstance();controller.setConfig({settings,playbackController:{getIsDynamic:()=>false}});controller.initialize();
 const bus=EventBus(context).getInstance();
 const add=(sample,mediaType='video')=>bus.trigger(MediaPlayerEvents.METRIC_ADDED,{metric:MetricsConstants.HTTP_REQUEST,mediaType,value:sample});
 return {context,settings,controller,add};
}
export function sample(serviceLocation,kbit,latency=50) {
 return {type:'MediaSegment',url:'https://media.invalid/segment.m4s',_serviceLocation:serviceLocation,trequest:new Date(0),tresponse:new Date(latency),_tfinish:new Date(latency+1000),_fileLoaderType:Constants.FILE_LOADER_TYPES.FETCH,trace:[{s:new Date(latency),d:1000,b:[kbit*125]}]};
}
