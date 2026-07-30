import AudioToolbox
import CoreAudioKit
import Foundation

@objc(ProductSwiftFactory)
final class ProductSwiftFactory: AUViewController, AUAudioUnitFactory {
    func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        throw NSError(domain: "auv3-product-factory", code: 1)
    }
}
