// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_1 | %FileCheck %s -check-prefix=UNRESOLVED_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_2 | %FileCheck %s -check-prefix=UNRESOLVED_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_3 | %FileCheck %s -check-prefix=UNRESOLVED_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_4 | %FileCheck %s -check-prefix=UNRESOLVED_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_5 | %FileCheck %s -check-prefix=UNRESOLVED_1

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_6 | %FileCheck %s -check-prefix=UNRESOLVED_2
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_7 | %FileCheck %s -check-prefix=UNRESOLVED_2
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_10 | %FileCheck %s -check-prefix=UNRESOLVED_2
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_11 | %FileCheck %s -check-prefix=UNRESOLVED_2

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_8 | %FileCheck %s -check-prefix=UNRESOLVED_3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_9 | %FileCheck %s -check-prefix=UNRESOLVED_3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_OPT_1 | %FileCheck %s -check-prefix=UNRESOLVED_3_OPT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_OPT_2 | %FileCheck %s -check-prefix=UNRESOLVED_3_OPT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_OPT_3 | %FileCheck %s -check-prefix=UNRESOLVED_3_OPTOPTOPT

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_12 | %FileCheck %s -check-prefix=UNRESOLVED_3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_13 | %FileCheck %s -check-prefix=UNRESOLVED_3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_14 | %FileCheck %s -check-prefix=UNRESOLVED_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_15 | %FileCheck %s -check-prefix=UNRESOLVED_1

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_16 | %FileCheck %s -check-prefix=UNRESOLVED_4
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_17 | %FileCheck %s -check-prefix=UNRESOLVED_4
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_18 | %FileCheck %s -check-prefix=UNRESOLVED_1

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_19 | %FileCheck %s -check-prefix=UNRESOLVED_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_20 | %FileCheck %s -check-prefix=UNRESOLVED_3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_21 | %FileCheck %s -check-prefix=UNRESOLVED_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_22 | %FileCheck %s -check-prefix=UNRESOLVED_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_23 | %FileCheck %s -check-prefix=UNRESOLVED_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_24 | %FileCheck %s -check-prefix=UNRESOLVED_3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_25 | %FileCheck %s -check-prefix=UNRESOLVED_3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_26 | %FileCheck %s -check-prefix=UNRESOLVED_3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_27 | %FileCheck %s -check-prefix=UNRESOLVED_3
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_27_NOCRASH > /dev/null
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_28 | %FileCheck %s -check-prefix=UNRESOLVED_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_29 | %FileCheck %s -check-prefix=UNRESOLVED_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=UNRESOLVED_30 | %FileCheck %s -check-prefix=UNRESOLVED_2

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=ENUM_AVAIL_1 | %FileCheck %s -check-prefix=ENUM_AVAIL_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=OPTIONS_AVAIL_1 | %FileCheck %s -check-prefix=OPTIONS_AVAIL_1

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=WITH_LITERAL_1 | %FileCheck %s -check-prefix=WITH_LITERAL_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=WITH_LITERAL_2 | %FileCheck %s -check-prefix=WITH_LITERAL_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=WITH_LITERAL_3 | %FileCheck %s -check-prefix=WITH_LITERAL_1

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=INVALID_1

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=OTHER_FILE_1 %S/Inputs/EnumFromOtherFile.swift | %FileCheck %s -check-prefix=OTHER_FILE_1

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=NON_OPT_SET_1 | %FileCheck %s -check-prefix=NON_OPT_SET_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=NON_OPT_SET_2 | %FileCheck %s -check-prefix=NON_OPT_SET_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=NON_OPT_SET_3 | %FileCheck %s -check-prefix=NON_OPT_SET_1

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=STRING_INTERPOLATION_1 | %FileCheck %s -check-prefix=STRING_INTERPOLATION_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=STRING_INTERPOLATION_INVALID

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=SUBTYPE_1 | %FileCheck %s -check-prefix=SUBTYPE_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=SUBTYPE_2 | %FileCheck %s -check-prefix=SUBTYPE_2

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=GENERIC_1 | %FileCheck %s -check-prefix=GENERIC_1 -check-prefix=GENERIC_1_INT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=GENERIC_2 | %FileCheck %s -check-prefix=GENERIC_1 -check-prefix=GENERIC_1_INT
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=GENERIC_3 | %FileCheck %s -check-prefix=GENERIC_1 -check-prefix=GENERIC_1_U

// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=STATIC_CLOSURE_1 | %FileCheck %s -check-prefix=STATIC_CLOSURE_1

enum SomeEnum1 {
  case South
  case North
}

enum SomeEnum2 {
  case East
  case West
}

enum SomeEnum3 {
  case Payload(SomeEnum1)
}

struct NotOptions1 {
  static let NotSet = 1
}

struct SomeOptions1 : OptionSet {
  let rawValue : Int
  static let Option1 = SomeOptions1(rawValue: 1 << 1)
  static let Option2 = SomeOptions1(rawValue: 1 << 2)
  static let Option3 = SomeOptions1(rawValue: 1 << 3)
  let NotStaticOption = SomeOptions1(rawValue: 1 << 4)
  static let NotOption = 1
}

struct SomeOptions2 : OptionSet {
  let rawValue : Int
  static let Option4 = SomeOptions2(rawValue: 1 << 1)
  static let Option5 = SomeOptions2(rawValue: 1 << 2)
  static let Option6 = SomeOptions2(rawValue: 1 << 3)
}

enum EnumAvail1 {
  case aaa
  @available(*, unavailable) case AAA
  @available(*, deprecated) case BBB
}

struct OptionsAvail1 : OptionSet {
  let rawValue: Int
  static let aaa = OptionsAvail1(rawValue: 1 << 0)
  @available(*, unavailable) static let AAA = OptionsAvail1(rawValue: 1 << 0)
  @available(*, deprecated) static let BBB = OptionsAvail1(rawValue: 1 << 1)
}

func OptionSetTaker1(_ Op : SomeOptions1) {}

func OptionSetTaker2(_ Op : SomeOptions2) {}

func OptionSetTaker3(_ Op1: SomeOptions1, _ Op2: SomeOptions2) {}

func OptionSetTaker4(_ Op1: SomeOptions2, _ Op2: SomeOptions1) {}

func OptionSetTaker5(_ Op1: SomeOptions1, _ Op2: SomeOptions2, _ En1 : SomeEnum1, _ En2: SomeEnum2) {}

func OptionSetTaker6(_ Op1: SomeOptions1, _ Op2: SomeOptions2) {}

func OptionSetTaker6(_ Op1: SomeOptions2, _ Op2: SomeOptions1) {}

func OptionSetTaker7(_ Op1: SomeOptions1, _ Op2: SomeOptions2) -> Int {return 0}

func EnumTaker1(_ E : SomeEnum1) {}
func optionalEnumTaker1(_ : SomeEnum1?) {}

class OptionTakerContainer1 {
  func OptionSetTaker1(_ op : SomeOptions1) {}
  func EnumTaker1(_ E : SomeEnum1) {}
}

class C1 {
  func f1() {
    var f : SomeOptions1
    f = .#^UNRESOLVED_1^#
  }
  func f2() {
    var f : SomeOptions1
    f = [.#^UNRESOLVED_2^#
  }
  func f3() {
    var f : SomeOptions1
    f = [.Option1, .#^UNRESOLVED_3^#
  }
}
class C2 {
  func f1() {
    OptionSetTaker1(.#^UNRESOLVED_4^#)
  }
  func f2() {
    OptionSetTaker1([.Option1, .#^UNRESOLVED_5^#])
  }
// UNRESOLVED_1:  Begin completions
// UNRESOLVED_1-NOT:  SomeEnum1
// UNRESOLVED_1-NOT:  SomeEnum2
// UNRESOLVED_1-DAG:  Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: Option1[#SomeOptions1#]; name=Option1
// UNRESOLVED_1-DAG:  Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: Option2[#SomeOptions1#]; name=Option2
// UNRESOLVED_1-DAG:  Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: Option3[#SomeOptions1#]; name=Option3
// UNRESOLVED_1-NOT:  Not
}

class C3 {
  func f1() {
    OptionSetTaker2(.#^UNRESOLVED_6^#)
  }
  func f2() {
    OptionSetTaker2([.Option4, .#^UNRESOLVED_7^#])
  }
// UNRESOLVED_2:  Begin completions
// UNRESOLVED_2-NOT:  SomeEnum1
// UNRESOLVED_2-NOT:  SomeEnum2
// UNRESOLVED_2-DAG:  Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: Option4[#SomeOptions2#]; name=Option4
// UNRESOLVED_2-DAG:  Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: Option5[#SomeOptions2#]; name=Option5
// UNRESOLVED_2-DAG:  Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: Option6[#SomeOptions2#]; name=Option6
// UNRESOLVED_2-NOT:  Not
}

class C4 {
  func f1() {
    var E : SomeEnum1
    E = .#^UNRESOLVED_8^#
  }
  func f2() {
    EnumTaker1(.#^UNRESOLVED_9^#)
  }
  func f3() {
    OptionSetTaker5(.Option1, .Option4, .#^UNRESOLVED_12^#, .West)
  }
  func f4() {
    var _: SomeEnum1? = .#^UNRESOLVED_OPT_1^#
  }
  func f5() {
    optionalEnumTaker1(.#^UNRESOLVED_OPT_2^#)
  }
  func f6() {
    var _: SomeEnum1??? = .#^UNRESOLVED_OPT_3^#
  }
}
// UNRESOLVED_3: Begin completions
// UNRESOLVED_3-DAG: Decl[EnumElement]/ExprSpecific:     North[#SomeEnum1#]; name=North
// UNRESOLVED_3-DAG: Decl[EnumElement]/ExprSpecific:     South[#SomeEnum1#]; name=South
// UNRESOLVED_3-NOT: SomeOptions1
// UNRESOLVED_3-NOT: SomeOptions2
// UNRESOLVED_3-NOT: none
// UNRESOLVED_3-NOT: some(

// UNRESOLVED_3_OPT: Begin completions
// UNRESOLVED_3_OPT-DAG: Decl[EnumElement]/ExprSpecific:     North[#SomeEnum1#];
// UNRESOLVED_3_OPT-DAG: Decl[EnumElement]/ExprSpecific:     South[#SomeEnum1#];
// UNRESOLVED_3_OPT-DAG: Decl[EnumElement]/ExprSpecific:     none[#Optional<Wrapped>#]; name=none
// UNRESOLVED_3_OPT-DAG: Decl[EnumElement]/ExprSpecific:     some({#Wrapped#})[#(Wrapped) -> Optional<Wrapped>#];
// UNRESOLVED_3_OPT-DAG: Decl[Constructor]/CurrNominal:      init({#(some): SomeEnum1#})[#Optional<SomeEnum1>#];
// UNRESOLVED_3_OPT-DAG: Decl[Constructor]/CurrNominal:      init({#nilLiteral: ()#})[#Optional<SomeEnum1>#];

// UNRESOLVED_3_OPTOPTOPT: Begin completions
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[EnumElement]/ExprSpecific:     North[#SomeEnum1#];
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[EnumElement]/ExprSpecific:     South[#SomeEnum1#];
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[EnumElement]/ExprSpecific:     none[#Optional<Wrapped>#]; name=none
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[EnumElement]/ExprSpecific:     some({#Wrapped#})[#(Wrapped) -> Optional<Wrapped>#];
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[Constructor]/CurrNominal:      init({#(some): SomeEnum1??#})[#Optional<SomeEnum1??>#];
// UNRESOLVED_3_OPTOPTOPT-DAG: Decl[Constructor]/CurrNominal:      init({#nilLiteral: ()#})[#Optional<SomeEnum1??>#];

class C5 {
  func f1() {
    OptionSetTaker3(.Option1, .#^UNRESOLVED_10^#)
  }
  func f2() {
    OptionSetTaker4(.#^UNRESOLVED_11^#, .Option1)
  }
}

OptionSetTaker5(.Option1, .Option4, .#^UNRESOLVED_13^#, .West)
OptionSetTaker5(.#^UNRESOLVED_14^#, .Option4, .South, .West)
OptionSetTaker5([.#^UNRESOLVED_15^#], .Option4, .South, .West)

OptionSetTaker6(.#^UNRESOLVED_16^#, .Option4)
OptionSetTaker6(.Option4, .#^UNRESOLVED_17^#,)

var a = {() in
  OptionSetTaker5([.#^UNRESOLVED_18^#], .Option4, .South, .West)
}
var Container = OptionTakerContainer1()
Container.OptionSetTaker1(.#^UNRESOLVED_19^#)
Container.EnumTaker1(.#^UNRESOLVED_20^#

func parserSync() {}

// UNRESOLVED_4: Begin completions
// UNRESOLVED_4-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: Option1[#SomeOptions1#]; name=Option1
// UNRESOLVED_4-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: Option2[#SomeOptions1#]; name=Option2
// UNRESOLVED_4-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: Option3[#SomeOptions1#]; name=Option3
// UNRESOLVED_4-NOT: Option4
// UNRESOLVED_4-NOT: Option5
// UNRESOLVED_4-NOT: Option6

var OpIns1 : SomeOptions1 = .#^UNRESOLVED_21^#

var c1 = {() -> SomeOptions1 in
  return .#^UNRESOLVED_22^#
}

class C6 {
  func f1() -> SomeOptions1 {
    return .#^UNRESOLVED_23^#
  }
  func f2(p : SomeEnum3) {
  switch p {
  case .Payload(.#^UNRESOLVED_24^#)
  }
  }
}

class C6 {
  func f1(e: SomeEnum1) {
    if let x = Optional(e) where x == .#^UNRESOLVED_25^#
  }
}
class C7 {}
extension C7 {
  func extendedf1(_ e :SomeEnum1) {}
}

var cInst1 = C7()
cInst1.extendedf1(.#^UNRESOLVED_26^#

func nocrash1() -> SomeEnum1 {
  return .#^UNRESOLVED_27_NOCRASH^#
}

func resetParser1() {}

func f() -> SomeEnum1 {
  return .#^UNRESOLVED_27^#
}

let TopLevelVar1 = OptionSetTaker7([.#^UNRESOLVED_28^#], Op2: [.Option4])

let TopLevelVar2 = OptionSetTaker1([.#^UNRESOLVED_29^#])

let TopLevelVar3 = OptionSetTaker7([.Option1], Op2: [.#^UNRESOLVED_30^#])

func testAvail1(_ x: EnumAvail1) {
  testAvail1(.#^ENUM_AVAIL_1^#)
}
// ENUM_AVAIL_1: Begin completions, 2 items
// ENUM_AVAIL_1-NOT: AAA
// ENUM_AVAIL_1-DAG: Decl[EnumElement]/ExprSpecific:     aaa[#EnumAvail1#];
// ENUM_AVAIL_1-DAG: Decl[EnumElement]/ExprSpecific/NotRecommended: BBB[#EnumAvail1#];
// ENUM_AVAIL_1-NOT: AAA
// ENUM_AVAIL_1: End completions

func testAvail2(_ x: OptionsAvail1) {
  testAvail2(.#^OPTIONS_AVAIL_1^#)
}
// OPTIONS_AVAIL_1: Begin completions
// ENUM_AVAIL_1-NOT: AAA
// OPTIONS_AVAIL_1-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]: aaa[#OptionsAvail1#];
// OPTIONS_AVAIL_1-DAG: Decl[StaticVar]/CurrNominal/NotRecommended/TypeRelation[Identical]: BBB[#OptionsAvail1#];
// OPTIONS_AVAIL_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Identical]: init({#rawValue: Int#})[#OptionsAvail1#]
// ENUM_AVAIL_1-NOT: AAA
// OPTIONS_AVAIL_1: End completions

func testWithLiteral1() {
  struct S {
    enum MyEnum { case myCase }
    enum Thing { case thingCase }
    var thing: Thing
    func takeEnum(thing: MyEnum, other: Double) {}
  }
  let s: S
  _ = s.takeEnum(thing: .#^WITH_LITERAL_1^#, other: 1.0)
// WITH_LITERAL_1: Begin completions, 1 items
// WITH_LITERAL_1-NEXT: Decl[EnumElement]/ExprSpecific:     myCase[#S.MyEnum#];
// WITH_LITERAL_1-NEXT: End completions
}
func testWithLiteral2() {
  struct S {
    enum MyEnum { case myCase }
    enum Thing { case thingCase }
    var thing: Thing
    func takeEnum(thing: MyEnum, other: Int) {}
    func takeEnum(thing: MyEnum, other: Double) {}
  }
  let s: S
  _ = s.takeEnum(thing: .#^WITH_LITERAL_2^#, other: 1.0)
}
func testWithLiteral3() {
  struct S {
    enum MyEnum { case myCase }
    enum Thing { case thingCase }
    var thing: Thing
    func takeEnum(thing: MyEnum, other: Int) {}
    func takeEnum(thing: MyEnum, other: Double) {}
    func test(s: S) {
      _ = s.takeEnum(thing: .#^WITH_LITERAL_3^#, other: 1.0)
    }
  }
}

func testInvalid1() {
  func invalid() -> NoSuchEnum {
    return .#^INVALID_1^# // Don't crash.
  }
}

func enumFromOtherFile() -> EnumFromOtherFile {
  return .#^OTHER_FILE_1^# // Don't crash.
}
// OTHER_FILE_1: Begin completions
// OTHER_FILE_1-DAG: Decl[EnumElement]/ExprSpecific:     b({#String#})[#(String) -> EnumFromOtherFile#];
// OTHER_FILE_1-DAG: Decl[EnumElement]/ExprSpecific:     a({#Int#})[#(Int) -> EnumFromOtherFile#];
// OTHER_FILE_1-DAG: Decl[EnumElement]/ExprSpecific:     c[#EnumFromOtherFile#];
// OTHER_FILE_1: End completions

struct NonOptSet {
  static let a = NonOptSet()
  static let wrongType = 1
  let notStatic = NonOptSet()
  init(x: Int, y: Int) {}
  init() {}
  static func b() -> NonOptSet { return NonOptSet() }
  static func wrongType() -> Int { return 0 }
  func notStatic() -> NonOptSet { return NonOptSet() }
}

func testNonOptSet() {
  let x: NonOptSet
  x = .#^NON_OPT_SET_1^#
}
// NON_OPT_SET_1: Begin completions, 4 items
// NON_OPT_SET_1-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Identical]:    a[#NonOptSet#]
// NON_OPT_SET_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Identical]:  init({#x: Int#}, {#y: Int#})[#NonOptSet#]
// NON_OPT_SET_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Identical]:  init()[#NonOptSet#]
// NON_OPT_SET_1-DAG: Decl[StaticMethod]/CurrNominal/TypeRelation[Identical]: b()[#NonOptSet#]
// NON_OPT_SET_1: End completions

func testNonOptSet() {
  let x: NonOptSet = .#^NON_OPT_SET_2^#
}

func testNonOptSet() -> NonOptSet {
  return .#^NON_OPT_SET_3^#
}

func testInStringInterpolation() {
  enum MyEnum { case foo, bar }
  func takeEnum(_ e: MyEnum) -> MyEnum { return e }
  let x = "enum: \(takeEnum(.#^STRING_INTERPOLATION_1^#))"
  let y = "enum: \(.#^STRING_INTERPOLATION_INVALID^#)" // Dont'crash.
}
// STRING_INTERPOLATION_1: Begin completions
// STRING_INTERPOLATION_1-DAG: Decl[EnumElement]/ExprSpecific:     foo[#MyEnum#];
// STRING_INTERPOLATION_1-DAG: Decl[EnumElement]/ExprSpecific:     bar[#MyEnum#];
// STRING_INTERPOLATION_1: End completions

class BaseClass {
  class SubClass : BaseClass { init() {} }
  static var subInstance: SubClass = SubClass()
}
protocol MyProtocol {
  typealias Concrete1 = BaseClass
  typealias Concrete2 = AnotherTy
}
extension BaseClass : MyProtocol {}
struct AnotherTy: MyProtocol {}
func testSubType() {
  var _: BaseClass = .#^SUBTYPE_1^#
}
// SUBTYPE_1: Begin completions, 4 items
// SUBTYPE_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Identical]: init()[#BaseClass#];
// SUBTYPE_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: SubClass()[#BaseClass.SubClass#];
// SUBTYPE_1-DAG: Decl[StaticVar]/CurrNominal/TypeRelation[Convertible]: subInstance[#BaseClass.SubClass#];
// SUBTYPE_1-DAG: Decl[Constructor]/Super/TypeRelation[Identical]: Concrete1()[#BaseClass#];
// SUBTYPE_1: End completions

func testMemberTypealias() {
  var _: MyProtocol = .#^SUBTYPE_2^#
}
// SUBTYPE_2: Begin completions, 2 items
// SUBTYPE_2-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: Concrete1()[#BaseClass#];
// SUBTYPE_2-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: Concrete2()[#AnotherTy#];
// SUBTYPE_2: End completions

enum Generic<T> {
  case contains(content: T)
  case empty
  static func create(_: T) -> Generic<T> { fatalError() }
}
func takeGenericInt(_: Generic<Int>) { }
func takeGenericU<U>(_: Generic<U>) { }
func testGeneric() {
  do {
    let _: Generic<Int> = .#^GENERIC_1^#
  }
  takeGenericInt(.#^GENERIC_2^#)
  takeGenericU(.#^GENERIC_3^#)
}
// GENERIC_1: Begin completions
// GENERIC_1:     Decl[EnumElement]/ExprSpecific:     contains({#content: T#})[#(T) -> Generic<T>#];
// GENERIC_1:     Decl[EnumElement]/ExprSpecific:     empty[#Generic<T>#];
// GENERIC_1_INT: Decl[StaticMethod]/CurrNominal:     create({#Int#})[#Generic<Int>#];
// GENERIC_1_U:   Decl[StaticMethod]/CurrNominal:     create({#U#})[#Generic<U>#];
// GENERIC_1: End completions

struct HasCreator {
  static var create: () -> HasCreator = { fatalError() }
  static var create_curried: () -> () -> HasCreator = { fatalError() }
}
func testHasStaticClosure() {
  let _: HasCreator = .#^STATIC_CLOSURE_1^#
}
// STATIC_CLOSURE_1: Begin completions, 2 items
// STATIC_CLOSURE_1-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Identical]: init()[#HasCreator#];
// FIXME: Suggest 'create()[#HasCreateor#]', not 'create'.
// STATIC_CLOSURE_1-DAG: Decl[StaticVar]/CurrNominal:        create[#() -> HasCreator#];
// STATIC_CLOSURE_1-NOT: create_curried
// STATIC_CLOSURE_1: End completions
