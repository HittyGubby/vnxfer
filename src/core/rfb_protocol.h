#pragma once

#include <cstdint>

namespace vnxfer::rfb {

// Security types
constexpr uint8_t kSecNone = 1;
constexpr uint8_t kSecVncAuth = 2;
constexpr uint8_t kSecTight = 16;

// Client -> server message types
constexpr uint8_t kMsgSetPixelFormat = 0;
constexpr uint8_t kMsgSetEncodings = 2;
constexpr uint8_t kMsgFramebufferUpdateRequest = 3;
constexpr uint8_t kMsgKeyEvent = 4;
constexpr uint8_t kMsgPointerEvent = 5;
constexpr uint8_t kMsgClientCutText = 6;

// Server -> client message types
constexpr uint8_t kMsgFramebufferUpdate = 0;
constexpr uint8_t kMsgSetColourMapEntries = 1;
constexpr uint8_t kMsgBell = 2;
constexpr uint8_t kMsgServerCutText = 3;
constexpr uint8_t kMsgFileTransfer = 7;

// UltraVNC file transfer content types
constexpr uint8_t kFtDirContentRequest = 1;
constexpr uint8_t kFtDirPacket = 2;
constexpr uint8_t kFtFileTransferRequest = 3;
constexpr uint8_t kFtFileHeader = 4;
constexpr uint8_t kFtFilePacket = 5;
constexpr uint8_t kFtEndOfFile = 6;
constexpr uint8_t kFtAbortFileTransfer = 7;
constexpr uint8_t kFtFileTransferOffer = 8;
constexpr uint8_t kFtFileAcceptHeader = 9;
constexpr uint8_t kFtCommand = 10;
constexpr uint8_t kFtCommandReturn = 11;
constexpr uint8_t kFtFileChecksums = 12;
constexpr uint8_t kFtFileTransferAccess = 14;
constexpr uint8_t kFtSessionStart = 15;
constexpr uint8_t kFtSessionEnd = 16;
constexpr uint8_t kFtProtocolVersion = 17;

// UltraVNC request/answer params
constexpr uint8_t kFtRDirContent = 1;
constexpr uint8_t kFtRDrivesList = 2;
constexpr uint8_t kFtADirectory = 1;
constexpr uint8_t kFtAFile = 2;
constexpr uint8_t kFtADrivesList = 3;

// TightVNC 1.3 extension messages
constexpr uint8_t kTv13FileListRequest = 130;
constexpr uint8_t kTv13FileListData = 130;
constexpr uint8_t kTv13FileDownloadRequest = 131;
constexpr uint8_t kTv13FileDownloadData = 131;
constexpr uint8_t kTv13FileUploadRequest = 132;
constexpr uint8_t kTv13FileUploadCancel = 132;
constexpr uint8_t kTv13FileUploadData = 133;
constexpr uint8_t kTv13FileDownloadFailed = 133;
constexpr uint8_t kTv13FileDownloadCancel = 134;
constexpr uint8_t kTv13FileUploadFailed = 135;
constexpr uint8_t kTv13FileCreateDirRequest = 136;

// TightVNC 2.x extension message ids
constexpr uint32_t kTv2CompressionSupportRequest = 0xFC000100;
constexpr uint32_t kTv2CompressionSupportReply = 0xFC000101;
constexpr uint32_t kTv2FileListRequest = 0xFC000102;
constexpr uint32_t kTv2FileListReply = 0xFC000103;
constexpr uint32_t kTv2Md5Request = 0xFC000104;
constexpr uint32_t kTv2Md5Reply = 0xFC000105;
constexpr uint32_t kTv2UploadStartRequest = 0xFC000106;
constexpr uint32_t kTv2UploadStartReply = 0xFC000107;
constexpr uint32_t kTv2UploadDataRequest = 0xFC000108;
constexpr uint32_t kTv2UploadDataReply = 0xFC000109;
constexpr uint32_t kTv2UploadEndRequest = 0xFC00010A;
constexpr uint32_t kTv2UploadEndReply = 0xFC00010B;
constexpr uint32_t kTv2DownloadStartRequest = 0xFC00010C;
constexpr uint32_t kTv2DownloadStartReply = 0xFC00010D;
constexpr uint32_t kTv2DownloadDataRequest = 0xFC00010E;
constexpr uint32_t kTv2DownloadDataReply = 0xFC00010F;
constexpr uint32_t kTv2DownloadEndReply = 0xFC000110;
constexpr uint32_t kTv2MkdirRequest = 0xFC000111;
constexpr uint32_t kTv2MkdirReply = 0xFC000112;
constexpr uint32_t kTv2RemoveRequest = 0xFC000113;
constexpr uint32_t kTv2RemoveReply = 0xFC000114;
constexpr uint32_t kTv2RenameRequest = 0xFC000115;
constexpr uint32_t kTv2RenameReply = 0xFC000116;
constexpr uint32_t kTv2DirSizeRequest = 0xFC000117;
constexpr uint32_t kTv2DirSizeReply = 0xFC000118;
constexpr uint32_t kTv2LastRequestFailedReply = 0xFC000119;

}  // namespace vnxfer::rfb
