SDK_DEMO_PATH ?= .
BL_SDK_BASE ?= $(SDK_DEMO_PATH)/../third_party/aithinker_Ai-M6X_SDK

export BL_SDK_BASE

# The Ai-M61-32S carries a BL618, which the SDK builds as the bl616 family.
CHIP ?= bl616
BOARD ?= bl616dk
CROSS_COMPILE ?= riscv64-unknown-elf-

include $(BL_SDK_BASE)/project.build
