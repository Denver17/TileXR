#
# Copyright (c) 2026 TileXR Project
#
# The direct CCU smoke runner compiles its probe against the active TileXR build
# so that it can test the exact libtile-comm.so under validation. Keep this
# directory free of private producer probe targets.
#

cmake_minimum_required(VERSION 3.16)
project(TileXR_CCU_Tests LANGUAGES CXX)
