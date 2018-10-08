//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if _runtime(_ObjC)

import SwiftShims

/// Equivalent to `NSDictionary.allKeys`, but does not leave objects on the
/// autorelease pool.
@inlinable
internal func _stdlib_NSDictionary_allKeys(
  _ nsd: _NSDictionary
) -> _HeapBuffer<Int, AnyObject> {
  let count = nsd.count
  let storage = _HeapBuffer<Int, AnyObject>(
    _HeapBufferStorage<Int, AnyObject>.self, count, count)

  nsd.getObjects(nil, andKeys: storage.baseAddress, count: count)
  return storage
}

extension _NativeDictionary { // Bridging
  @usableFromInline
  __consuming internal func bridged() -> _NSDictionary {
    // We can zero-cost bridge if our keys are verbatim
    // or if we're the empty singleton.

    // Temporary var for SOME type safety before a cast.
    let nsDictionary: _NSDictionaryCore

    if _storage === _RawDictionaryStorage.empty || count == 0 {
      nsDictionary = _RawDictionaryStorage.empty
    } else if _isBridgedVerbatimToObjectiveC(Key.self),
      _isBridgedVerbatimToObjectiveC(Value.self) {
      nsDictionary = unsafeDowncast(
        _storage,
        to: _DictionaryStorage<Key, Value>.self)
    } else {
      nsDictionary = _SwiftDeferredNSDictionary(self)
    }

    // Cast from "minimal NSDictionary" to "NSDictionary"
    // Note that if you actually ask Swift for this cast, it will fail.
    // Never trust a shadow protocol!
    return unsafeBitCast(nsDictionary, to: _NSDictionary.self)
  }
}

/// An NSEnumerator that works with any _NativeDictionary of
/// verbatim bridgeable elements. Used by the various NSDictionary impls.
final internal class _SwiftDictionaryNSEnumerator<Key: Hashable, Value>
  : __SwiftNativeNSEnumerator, _NSEnumerator {

  @nonobjc internal var base: _NativeDictionary<Key, Value>
  @nonobjc internal var bridgedKeys: _BridgingHashBuffer?
  @nonobjc internal var nextBucket: _NativeDictionary<Key, Value>.Bucket
  @nonobjc internal var endBucket: _NativeDictionary<Key, Value>.Bucket

  @objc
  internal override required init() {
    _sanityCheckFailure("don't call this designated initializer")
  }

  internal init(_ base: __owned _NativeDictionary<Key, Value>) {
    _sanityCheck(_isBridgedVerbatimToObjectiveC(Key.self))
    self.base = base
    self.bridgedKeys = nil
    self.nextBucket = base.hashTable.startBucket
    self.endBucket = base.hashTable.endBucket
  }

  @nonobjc
  internal init(_ deferred: __owned _SwiftDeferredNSDictionary<Key, Value>) {
    _sanityCheck(!_isBridgedVerbatimToObjectiveC(Key.self))
    self.base = deferred.native
    self.bridgedKeys = deferred.bridgeKeys()
    self.nextBucket = base.hashTable.startBucket
    self.endBucket = base.hashTable.endBucket
  }

  private func bridgedKey(at bucket: _HashTable.Bucket) -> AnyObject {
    _sanityCheck(base.hashTable.isOccupied(bucket))
    if let bridgedKeys = self.bridgedKeys {
      return bridgedKeys[bucket]
    }
    return _bridgeAnythingToObjectiveC(base.uncheckedKey(at: bucket))
  }

  @objc
  internal func nextObject() -> AnyObject? {
    if nextBucket == endBucket {
      return nil
    }
    let bucket = nextBucket
    nextBucket = base.hashTable.occupiedBucket(after: nextBucket)
    return self.bridgedKey(at: bucket)
  }

  @objc(countByEnumeratingWithState:objects:count:)
  internal func countByEnumerating(
    with state: UnsafeMutablePointer<_SwiftNSFastEnumerationState>,
    objects: UnsafeMutablePointer<AnyObject>,
    count: Int
  ) -> Int {
    var theState = state.pointee
    if theState.state == 0 {
      theState.state = 1 // Arbitrary non-zero value.
      theState.itemsPtr = AutoreleasingUnsafeMutablePointer(objects)
      theState.mutationsPtr = _fastEnumerationStorageMutationsPtr
    }

    if nextBucket == endBucket {
      state.pointee = theState
      return 0
    }

    // Return only a single element so that code can start iterating via fast
    // enumeration, terminate it, and continue via NSEnumerator.
    let unmanagedObjects = _UnmanagedAnyObjectArray(objects)
    unmanagedObjects[0] = self.bridgedKey(at: nextBucket)
    nextBucket = base.hashTable.occupiedBucket(after: nextBucket)
    state.pointee = theState
    return 1
  }
}

/// This class exists for Objective-C bridging. It holds a reference to a
/// _NativeDictionary, and can be upcast to NSSelf when bridging is
/// necessary.  This is the fallback implementation for situations where
/// toll-free bridging isn't possible. On first access, a _NativeDictionary
/// of AnyObject will be constructed containing all the bridged elements.
final internal class _SwiftDeferredNSDictionary<Key: Hashable, Value>
  : __SwiftNativeNSDictionary, _NSDictionaryCore {

  @usableFromInline
  internal typealias Bucket = _HashTable.Bucket

  // This stored property must be stored at offset zero.  We perform atomic
  // operations on it.
  //
  // Do not access this property directly.
  @nonobjc
  private var _bridgedKeys_DoNotUse: AnyObject?

  // This stored property must be stored at offset one.  We perform atomic
  // operations on it.
  //
  // Do not access this property directly.
  @nonobjc
  private var _bridgedValues_DoNotUse: AnyObject?

  /// The unbridged elements.
  internal var native: _NativeDictionary<Key, Value>

  internal init(_ native: __owned _NativeDictionary<Key, Value>) {
    _sanityCheck(native.count > 0)
    _sanityCheck(!_isBridgedVerbatimToObjectiveC(Key.self) ||
      !_isBridgedVerbatimToObjectiveC(Value.self))
    self.native = native
    super.init()
  }

  @objc
  internal required init(
    objects: UnsafePointer<AnyObject?>,
    forKeys: UnsafeRawPointer,
    count: Int
  ) {
    _sanityCheckFailure("don't call this designated initializer")
  }

  @nonobjc
  private var _bridgedKeysPtr: UnsafeMutablePointer<AnyObject?> {
    return _getUnsafePointerToStoredProperties(self)
      .assumingMemoryBound(to: Optional<AnyObject>.self)
  }

  @nonobjc
  private var _bridgedValuesPtr: UnsafeMutablePointer<AnyObject?> {
    return _bridgedKeysPtr + 1
  }

  /// The buffer for bridged keys, if present.
  @nonobjc
  private var _bridgedKeys: _BridgingHashBuffer? {
    guard let ref = _stdlib_atomicLoadARCRef(object: _bridgedKeysPtr) else {
      return nil
    }
    return unsafeDowncast(ref, to: _BridgingHashBuffer.self)
  }

  /// The buffer for bridged values, if present.
  @nonobjc
  private var _bridgedValues: _BridgingHashBuffer? {
    guard let ref = _stdlib_atomicLoadARCRef(object: _bridgedValuesPtr) else {
      return nil
    }
    return unsafeDowncast(ref, to: _BridgingHashBuffer.self)
  }

  /// Attach a buffer for bridged Dictionary keys.
  @nonobjc
  private func _initializeBridgedKeys(_ storage: _BridgingHashBuffer) {
    _stdlib_atomicInitializeARCRef(object: _bridgedKeysPtr, desired: storage)
  }

  /// Attach a buffer for bridged Dictionary values.
  @nonobjc
  private func _initializeBridgedValues(_ storage: _BridgingHashBuffer) {
    _stdlib_atomicInitializeARCRef(object: _bridgedValuesPtr, desired: storage)
  }

  @nonobjc
  internal func bridgeKeys() -> _BridgingHashBuffer? {
    if _isBridgedVerbatimToObjectiveC(Key.self) { return nil }
    if let bridgedKeys = _bridgedKeys { return bridgedKeys }

    // Allocate and initialize heap storage for bridged keys.
    let bridged = _BridgingHashBuffer.allocate(
      owner: native._storage,
      hashTable: native.hashTable)
    for bucket in native.hashTable {
      let object = _bridgeAnythingToObjectiveC(native.uncheckedKey(at: bucket))
      bridged.initialize(at: bucket, to: object)
    }

    // Atomically put the bridged keys in place.
    _initializeBridgedKeys(bridged)
    return _bridgedKeys!
  }

  @nonobjc
  internal func bridgeValues() -> _BridgingHashBuffer? {
    if _isBridgedVerbatimToObjectiveC(Value.self) { return nil }
    if let bridgedValues = _bridgedValues { return bridgedValues }

    // Allocate and initialize heap storage for bridged values.
    let bridged = _BridgingHashBuffer.allocate(
      owner: native._storage,
      hashTable: native.hashTable)
    for bucket in native.hashTable {
      let value = native.uncheckedValue(at: bucket)
      let cocoaValue = _bridgeAnythingToObjectiveC(value)
      bridged.initialize(at: bucket, to: cocoaValue)
    }

    // Atomically put the bridged values in place.
    _initializeBridgedValues(bridged)
    return _bridgedValues!
  }

  @objc(copyWithZone:)
  internal func copy(with zone: _SwiftNSZone?) -> AnyObject {
    // Instances of this class should be visible outside of standard library as
    // having `NSDictionary` type, which is immutable.
    return self
  }

  @inline(__always)
  private func _key(
    at bucket: Bucket,
    bridgedKeys: _BridgingHashBuffer?
  ) -> AnyObject {
    if let bridgedKeys = bridgedKeys {
      return bridgedKeys[bucket]
    }
    return _bridgeAnythingToObjectiveC(native.uncheckedKey(at: bucket))
  }

  @inline(__always)
  private func _value(
    at bucket: Bucket,
    bridgedValues: _BridgingHashBuffer?
  ) -> AnyObject {
    if let bridgedValues = bridgedValues {
      return bridgedValues[bucket]
    }
    return _bridgeAnythingToObjectiveC(native.uncheckedValue(at: bucket))
  }

  @objc(objectForKey:)
  internal func object(forKey aKey: AnyObject) -> AnyObject? {
    guard let nativeKey = _conditionallyBridgeFromObjectiveC(aKey, Key.self)
    else { return nil }

    let (bucket, found) = native.find(nativeKey)
    guard found else { return nil }
    return _value(at: bucket, bridgedValues: bridgeValues())
  }

  @objc
  internal func keyEnumerator() -> _NSEnumerator {
    if _isBridgedVerbatimToObjectiveC(Key.self) {
      return _SwiftDictionaryNSEnumerator<Key, Value>(native)
    }
    return _SwiftDictionaryNSEnumerator<Key, Value>(self)
  }

  @objc(getObjects:andKeys:count:)
  internal func getObjects(
    _ objects: UnsafeMutablePointer<AnyObject>?,
    andKeys keys: UnsafeMutablePointer<AnyObject>?,
    count: Int
  ) {
    _precondition(count >= 0, "Invalid count")
    guard count > 0 else { return }
    let bridgedKeys = bridgeKeys()
    let bridgedValues = bridgeValues()
    var i = 0 // Current position in the output buffers
    let bucketCount = native._storage._bucketCount

    defer { _fixLifetime(self) }

    switch (_UnmanagedAnyObjectArray(keys), _UnmanagedAnyObjectArray(objects)) {
    case (let unmanagedKeys?, let unmanagedObjects?):
      for bucket in native.hashTable {
        unmanagedKeys[i] = _key(at: bucket, bridgedKeys: bridgedKeys)
        unmanagedObjects[i] = _value(at: bucket, bridgedValues: bridgedValues)
        i += 1
        guard i < count else { break }
      }
    case (let unmanagedKeys?, nil):
      for bucket in native.hashTable {
        unmanagedKeys[i] = _key(at: bucket, bridgedKeys: bridgedKeys)
        i += 1
        guard i < count else { break }
      }
    case (nil, let unmanagedObjects?):
      for bucket in native.hashTable {
        unmanagedObjects[i] = _value(at: bucket, bridgedValues: bridgedValues)
        i += 1
        guard i < count else { break }
      }
    case (nil, nil):
      // Do nothing
      break
    }
  }

  @objc(enumerateKeysAndObjectsWithOptions:usingBlock:)
  internal func enumerateKeysAndObjects(
    options: Int,
    using block: @convention(block) (
      Unmanaged<AnyObject>,
      Unmanaged<AnyObject>,
      UnsafeMutablePointer<UInt8>
    ) -> Void) {
    let bridgedKeys = bridgeKeys()
    let bridgedValues = bridgeValues()

    defer { _fixLifetime(self) }

    var stop: UInt8 = 0
    for bucket in native.hashTable {
      let key = _key(at: bucket, bridgedKeys: bridgedKeys)
      let value = _value(at: bucket, bridgedValues: bridgedValues)
      block(
        Unmanaged.passUnretained(key),
        Unmanaged.passUnretained(value),
        &stop)
      if stop != 0 { return }
    }
  }

  @objc
  internal var count: Int {
    return native.count
  }

  @objc(countByEnumeratingWithState:objects:count:)
  internal func countByEnumerating(
    with state: UnsafeMutablePointer<_SwiftNSFastEnumerationState>,
    objects: UnsafeMutablePointer<AnyObject>?,
    count: Int
  ) -> Int {
    defer { _fixLifetime(self) }
    let hashTable = native.hashTable

    var theState = state.pointee
    if theState.state == 0 {
      theState.state = 1 // Arbitrary non-zero value.
      theState.itemsPtr = AutoreleasingUnsafeMutablePointer(objects)
      theState.mutationsPtr = _fastEnumerationStorageMutationsPtr
      theState.extra.0 = CUnsignedLong(hashTable.startBucket.offset)
    }

    // Test 'objects' rather than 'count' because (a) this is very rare anyway,
    // and (b) the optimizer should then be able to optimize away the
    // unwrapping check below.
    if _slowPath(objects == nil) {
      return 0
    }

    let unmanagedObjects = _UnmanagedAnyObjectArray(objects!)
    var bucket = _HashTable.Bucket(offset: Int(theState.extra.0))
    let endBucket = hashTable.endBucket
    _precondition(bucket == endBucket || hashTable.isOccupied(bucket),
      "Invalid fast enumeration state")
    var stored = 0

    // Only need to bridge once, so we can hoist it out of the loop.
    let bridgedKeys = bridgeKeys()
    for i in 0..<count {
      if bucket == endBucket { break }

      unmanagedObjects[i] = _key(at: bucket, bridgedKeys: bridgedKeys)
      stored += 1
      bucket = hashTable.occupiedBucket(after: bucket)
    }
    theState.extra.0 = CUnsignedLong(bucket.offset)
    state.pointee = theState
    return stored
  }
}

@usableFromInline
@_fixed_layout
internal struct _CocoaDictionary {
  @usableFromInline
  internal let object: _NSDictionary

  @inlinable
  internal init(_ object: __owned _NSDictionary) {
    self.object = object
  }
}

extension _CocoaDictionary {
  @usableFromInline
  internal func isEqual(to other: _CocoaDictionary) -> Bool {
    return _stdlib_NSObject_isEqual(self.object, other.object)
  }
}

extension _CocoaDictionary: _DictionaryBuffer {
  @usableFromInline
  internal typealias Key = AnyObject
  @usableFromInline
  internal typealias Value = AnyObject

  @usableFromInline // FIXME(cocoa-index): Should be inlinable
  internal var startIndex: Index {
    @_effects(releasenone)
    get {
      let allKeys = _stdlib_NSDictionary_allKeys(self.object)
      return Index(Index.Storage(self, allKeys, 0))
    }
  }

  @usableFromInline // FIXME(cocoa-index): Should be inlinable
  internal var endIndex: Index {
    @_effects(releasenone)
    get {
      let allKeys = _stdlib_NSDictionary_allKeys(self.object)
      return Index(Index.Storage(self, allKeys, allKeys.value))
    }
  }

  @usableFromInline // FIXME(cocoa-index): Should be inlinable
  @_effects(releasenone)
  internal func index(after index: Index) -> Index {
    var result = index.copy()
    formIndex(after: &result, isUnique: true)
    return result
  }

  internal func validate(_ index: Index) {
    _precondition(index.storage.base.object === self.object, "Invalid index")
    _precondition(index.storage.currentKeyIndex < index.storage.allKeys.value,
      "Attempt to access endIndex")
  }

  @usableFromInline // FIXME(cocoa-index): Should be inlinable
  internal func formIndex(after index: inout Index, isUnique: Bool) {
    validate(index)
    if !isUnique { index = index.copy() }
    let storage = index.storage // FIXME: rdar://problem/44863751
    storage.currentKeyIndex += 1
  }

  @usableFromInline // FIXME(cocoa-index): Should be inlinable
  @_effects(releasenone)
  internal func index(forKey key: Key) -> Index? {
    // Fast path that does not involve creating an array of all keys.  In case
    // the key is present, this lookup is a penalty for the slow path, but the
    // potential savings are significant: we could skip a memory allocation and
    // a linear search.
    if lookup(key) == nil {
      return nil
    }

    let allKeys = _stdlib_NSDictionary_allKeys(object)
    for i in 0..<allKeys.value {
      if _stdlib_NSObject_isEqual(key, allKeys[i]) {
        return Index(Index.Storage(self, allKeys, i))
      }
    }
    _sanityCheckFailure(
      "An NSDictionary key wassn't listed amongst its enumerated contents")
  }

  @inlinable
  internal var count: Int {
    return object.count
  }

  @inlinable
  @inline(__always)
  internal func contains(_ key: Key) -> Bool {
    return object.object(forKey: key) != nil
  }

  @inlinable
  @inline(__always)
  internal func lookup(_ key: Key) -> Value? {
    return object.object(forKey: key)
  }

  @usableFromInline // FIXME(cocoa-index): Should be inlinable
  @_effects(releasenone)
  internal func lookup(_ index: Index) -> (key: Key, value: Value) {
    _precondition(index.storage.base.object === self.object, "Invalid index")
    let key: Key = index.storage.allKeys[index.storage.currentKeyIndex]
    let value: Value = index.storage.base.object.object(forKey: key)!
    return (key, value)
  }

  @usableFromInline // FIXME(cocoa-index): Make inlinable
  @_effects(releasenone)
  func key(at index: Index) -> Key {
    _precondition(index.storage.base.object === self.object, "Invalid index")
    return index.key
  }

  @usableFromInline // FIXME(cocoa-index): Make inlinable
  @_effects(releasenone)
  func value(at index: Index) -> Value {
    _precondition(index.storage.base.object === self.object, "Invalid index")
    let key = index.storage.allKeys[index.storage.currentKeyIndex]
    return index.storage.base.object.object(forKey: key)!
  }
}

extension _CocoaDictionary {
  @inlinable
  internal func mapValues<Key: Hashable, Value, T>(
    _ transform: (Value) throws -> T
  ) rethrows -> _NativeDictionary<Key, T> {
    var result = _NativeDictionary<Key, T>(capacity: self.count)
    for (cocoaKey, cocoaValue) in self {
      let key = _forceBridgeFromObjectiveC(cocoaKey, Key.self)
      let value = _forceBridgeFromObjectiveC(cocoaValue, Value.self)
      try result.insertNew(key: key, value: transform(value))
    }
    return result
  }
}

extension _CocoaDictionary {
  @_fixed_layout
  @usableFromInline
  internal struct Index {
    @usableFromInline
    internal var _object: Builtin.BridgeObject
    @usableFromInline
    internal var _storage: Builtin.BridgeObject

    internal var object: AnyObject {
      @inline(__always)
      get {
        return _bridgeObject(toNonTaggedObjC: _object)
      }
    }

    internal var storage: Storage {
      @inline(__always)
      get {
        let storage = _bridgeObject(toNative: _storage)
        return unsafeDowncast(storage, to: Storage.self)
      }
    }

    internal init(_ storage: Storage) {
      self._object = _bridgeObject(fromNonTaggedObjC: storage.base.object)
      self._storage = _bridgeObject(fromNative: storage)
    }
  }
}

extension _CocoaDictionary.Index {
  // FIXME(cocoa-index): Try using an NSEnumerator to speed this up.
  internal class Storage {
  // Assumption: we rely on NSDictionary.getObjects when being
    // repeatedly called on the same NSDictionary, returning items in the same
    // order every time.
    // Similarly, the same assumption holds for NSSet.allObjects.

    /// A reference to the NSDictionary, which owns members in `allObjects`,
    /// or `allKeys`, for NSSet and NSDictionary respectively.
    internal let base: _CocoaDictionary
    // FIXME: swift-3-indexing-model: try to remove the cocoa reference, but
    // make sure that we have a safety check for accessing `allKeys`.  Maybe
    // move both into the dictionary/set itself.

    /// An unowned array of keys.
    internal var allKeys: _HeapBuffer<Int, AnyObject>

    /// Index into `allKeys`
    internal var currentKeyIndex: Int

    internal init(
      _ base: __owned _CocoaDictionary,
      _ allKeys: __owned _HeapBuffer<Int, AnyObject>,
      _ currentKeyIndex: Int
    ) {
      self.base = base
      self.allKeys = allKeys
      self.currentKeyIndex = currentKeyIndex
    }
  }
}

extension _CocoaDictionary.Index {
  @usableFromInline
  internal var handleBitPattern: UInt {
    @_effects(readonly)
    get {
      return unsafeBitCast(storage, to: UInt.self)
    }
  }

  @usableFromInline
  internal var dictionary: _CocoaDictionary {
    @_effects(releasenone)
    get {
      return storage.base
    }
  }

  @usableFromInline
  internal func copy() -> _CocoaDictionary.Index {
    let storage = self.storage
    return _CocoaDictionary.Index(Storage(
        storage.base,
        storage.allKeys,
        storage.currentKeyIndex))
  }
}

extension _CocoaDictionary.Index {
  @usableFromInline // FIXME(cocoa-index): Make inlinable
  @nonobjc
  internal var key: AnyObject {
    @_effects(readonly)
    get {
      _precondition(storage.currentKeyIndex < storage.allKeys.value,
        "Attempting to access Dictionary elements using an invalid index")
      return storage.allKeys[storage.currentKeyIndex]
    }
  }

  @usableFromInline // FIXME(cocoa-index): Make inlinable
  @nonobjc
  internal var age: Int32 {
    @_effects(readonly)
    get {
      return _HashTable.age(for: object)
    }
  }
}

extension _CocoaDictionary.Index: Equatable {
  @usableFromInline // FIXME(cocoa-index): Make inlinable
  @_effects(readonly)
  internal static func == (
    lhs: _CocoaDictionary.Index,
    rhs: _CocoaDictionary.Index
  ) -> Bool {
    _precondition(lhs.storage.base.object === rhs.storage.base.object,
      "Comparing indexes from different dictionaries")
    return lhs.storage.currentKeyIndex == rhs.storage.currentKeyIndex
  }
}

extension _CocoaDictionary.Index: Comparable {
  @usableFromInline // FIXME(cocoa-index): Make inlinable
  @_effects(readonly)
  internal static func < (
    lhs: _CocoaDictionary.Index,
    rhs: _CocoaDictionary.Index
  ) -> Bool {
    _precondition(lhs.storage.base.object === rhs.storage.base.object,
      "Comparing indexes from different dictionaries")
    return lhs.storage.currentKeyIndex < rhs.storage.currentKeyIndex
  }
}

extension _CocoaDictionary: Sequence {
  @usableFromInline
  final internal class Iterator {
    // Cocoa Dictionary iterator has to be a class, otherwise we cannot
    // guarantee that the fast enumeration struct is pinned to a certain memory
    // location.

    // This stored property should be stored at offset zero.  There's code below
    // relying on this.
    internal var _fastEnumerationState: _SwiftNSFastEnumerationState =
      _makeSwiftNSFastEnumerationState()

    // This stored property should be stored right after
    // `_fastEnumerationState`.  There's code below relying on this.
    internal var _fastEnumerationStackBuf = _CocoaFastEnumerationStackBuf()

    internal let base: _CocoaDictionary

    internal var _fastEnumerationStatePtr:
      UnsafeMutablePointer<_SwiftNSFastEnumerationState> {
      return _getUnsafePointerToStoredProperties(self).assumingMemoryBound(
        to: _SwiftNSFastEnumerationState.self)
    }

    internal var _fastEnumerationStackBufPtr:
      UnsafeMutablePointer<_CocoaFastEnumerationStackBuf> {
      return UnsafeMutableRawPointer(_fastEnumerationStatePtr + 1)
      .assumingMemoryBound(to: _CocoaFastEnumerationStackBuf.self)
    }

    // These members have to be word-sized integers, they cannot be limited to
    // Int8 just because our storage holds 16 elements: fast enumeration is
    // allowed to return inner pointers to the container, which can be much
    // larger.
    internal var itemIndex: Int = 0
    internal var itemCount: Int = 0

    internal init(_ base: __owned _CocoaDictionary) {
      self.base = base
    }
  }

  @usableFromInline
  @_effects(releasenone)
  internal __consuming func makeIterator() -> Iterator {
    return Iterator(self)
  }
}

extension _CocoaDictionary.Iterator: IteratorProtocol {
  @usableFromInline
  internal typealias Element = (key: AnyObject, value: AnyObject)

  @usableFromInline
  internal func nextKey() -> AnyObject? {
    if itemIndex < 0 {
      return nil
    }
    let base = self.base
    if itemIndex == itemCount {
      let stackBufCount = _fastEnumerationStackBuf.count
      // We can't use `withUnsafeMutablePointer` here to get pointers to
      // properties, because doing so might introduce a writeback storage, but
      // fast enumeration relies on the pointer identity of the enumeration
      // state struct.
      itemCount = base.object.countByEnumerating(
        with: _fastEnumerationStatePtr,
        objects: UnsafeMutableRawPointer(_fastEnumerationStackBufPtr)
          .assumingMemoryBound(to: AnyObject.self),
        count: stackBufCount)
      if itemCount == 0 {
        itemIndex = -1
        return nil
      }
      itemIndex = 0
    }
    let itemsPtrUP =
    UnsafeMutableRawPointer(_fastEnumerationState.itemsPtr!)
      .assumingMemoryBound(to: AnyObject.self)
    let itemsPtr = _UnmanagedAnyObjectArray(itemsPtrUP)
    let key: AnyObject = itemsPtr[itemIndex]
    itemIndex += 1
    return key
  }

  @usableFromInline
  internal func next() -> Element? {
    guard let key = nextKey() else { return nil }
    let value: AnyObject = base.object.object(forKey: key)!
    return (key, value)
  }
}

//===--- Bridging ---------------------------------------------------------===//

extension Dictionary {
  @inlinable
  public __consuming func _bridgeToObjectiveCImpl() -> _NSDictionaryCore {
    guard _variant.isNative else {
      return _variant.asCocoa.object
    }
    return _variant.asNative.bridged()
  }

  /// Returns the native Dictionary hidden inside this NSDictionary;
  /// returns nil otherwise.
  public static func _bridgeFromObjectiveCAdoptingNativeStorageOf(
    _ s: __owned AnyObject
  ) -> Dictionary<Key, Value>? {

    // Try all three NSDictionary impls that we currently provide.

    if let deferred = s as? _SwiftDeferredNSDictionary<Key, Value> {
      return Dictionary(_native: deferred.native)
    }

    if let nativeStorage = s as? _DictionaryStorage<Key, Value> {
      return Dictionary(_native: _NativeDictionary(nativeStorage))
    }

    if s === _RawDictionaryStorage.empty {
      return Dictionary()
    }

    // FIXME: what if `s` is native storage, but for different key/value type?
    return nil
  }
}

#endif // _runtime(_ObjC)
