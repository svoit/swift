// RUN: %target-swift-frontend -primary-file %s -emit-ir | %FileCheck %s

// CHECK: @"$s10empty_enum6JamaisOMf" =
//   CHECK-SAME: @"$sytWV"

enum Jamais {}
