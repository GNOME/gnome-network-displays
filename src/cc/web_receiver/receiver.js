const context = cast.framework.CastReceiverContext.getInstance();
const playerManager = context.getPlayerManager();

context.setLoggerLevel(cast.framework.LoggerLevel.DEBUG);

// Debug Logger
const castDebugLogger = cast.debug.CastDebugLogger.getInstance();
const LOG_TAG = 'MyAPP.LOG';

// Enable debug logger and show a 'DEBUG MODE' overlay at top left corner.
castDebugLogger.setEnabled(true);

// Show debug overlay
castDebugLogger.showDebugLogs(true);

// Set verbosity level for Core events.
castDebugLogger.loggerLevelByEvents = {
  'cast.framework.events.category.CORE': cast.framework.LoggerLevel.DEBUG,
  'cast.framework.events.EventType.MEDIA_STATUS': cast.framework.LoggerLevel.DEBUG
}

// Set verbosity level for custom tags.
// castDebugLogger.loggerLevelByTags = {
//     [LOG_TAG]: cast.framework.LoggerLevel.DEBUG,
// };

context.start();

