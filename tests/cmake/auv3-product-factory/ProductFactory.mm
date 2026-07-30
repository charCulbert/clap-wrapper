#import "detail/auv3/auv3_audiounit.h"

@interface ProductFactory : ClapAUv3ViewController
@end

@implementation ProductFactory

- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error
{
  ClapAUv3AudioUnit *au = [[ClapAUv3AudioUnit alloc] initWithComponentDescription:desc
                                                                            options:0
                                                                              error:error
                                                                           clapName:@"Fixture"
                                                                             clapId:@""
                                                                          clapIndex:0];
  self.audioUnit = au;
  return au;
}

@end
