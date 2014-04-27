#include "http_thread_tizen.hpp"
#include"../base/logging.hpp"

#include "platform.hpp"
#include "http_thread_callback.hpp"
#include "tizen_string_utils.hpp"

#include <FBaseColIEnumerator.h>

using namespace std;
using namespace Tizen::Net::Http;

using Tizen::Base::String;
using Tizen::Base::LongLong;
using Tizen::Base::ByteBuffer;
using Tizen::Base::Collection::IEnumerator;

HttpThread::HttpThread(std::string const & url,
                       downloader::IHttpThreadCallback & callback,
                       int64_t beg,
                       int64_t end,
                       int64_t size,
                       string const & pb)
  : m_callback(callback),
    m_begRange(beg), m_endRange(end),
    m_downloadedBytes(0), m_expectedSize(size),
    m_url(url), m_pb(pb),
    m_pTransaction(null)
{
  result r = E_SUCCESS;

  String * pProxyAddr = null;
  String hostAddr(m_url.c_str());
  HttpHeader * header = NULL;

  LOG(LDEBUG, ("Creating HttpSession", m_url));
  m_pSession = new HttpSession();
  r = m_pSession->Construct(NET_HTTP_SESSION_MODE_NORMAL, pProxyAddr, hostAddr, header);
  if (r != E_SUCCESS)
  {
    LOG(LERROR, ("HttpSession Construction error:", r));
    return;
  }

  // Open a new HttpTransaction.
  m_pTransaction = m_pSession->OpenTransactionN();
  if ((r = GetLastResult()) != E_SUCCESS)
  {
    LOG(LERROR, ("OpenTransactionN", GetLastResult()));
    return;
  }

  m_pTransaction->AddHttpTransactionListener(*this);
  m_pTransaction->SetHttpProgressListener(*this);

  HttpRequest* pRequest = m_pTransaction->GetRequest();
  pRequest->SetUri(m_url.c_str());

  HttpHeader* pHeader = pRequest->GetHeader();

  // use Range header only if we don't download whole file from start
  if (!(m_begRange == 0 && m_endRange < 0))
  {
    if (m_endRange > 0)
    {
      LOG(LDEBUG, (m_url, "downloading range [", m_begRange, ",", m_endRange, "]"));
      String range("bytes=");
      range.Append(m_begRange);
      range.Append('-');
      range.Append(m_endRange);
      pHeader->AddField(L"Range", range);
    }
    else
    {
      LOG(LDEBUG, (m_url, "resuming download from position", m_begRange));
      String range("bytes=");
      range.Append(m_begRange);
      range.Append('-');
      pHeader->AddField("Range", range);
    }
  }

  // set user-agent with unique client id only for mapswithme requests
  if (m_url.find("mapswithme.com") != string::npos)
  {
    static string const uid = GetPlatform().UniqueClientId();
    pHeader->AddField("User-Agent", uid.c_str());
  }

  if (m_pb.empty())
  {
    pRequest->SetMethod(NET_HTTP_METHOD_GET);
  }
  else
  {
    pRequest->SetMethod(NET_HTTP_METHOD_POST);
    pHeader->AddField("Content-Type", "application/json");
    long  long sz = m_pb.size();
    String lenght;
    lenght.Append(sz);
    pHeader->AddField("Content-Length", lenght);
    ByteBuffer body;
    body.Construct((const byte *)m_pb.c_str(), 0, sz, sz);
    pRequest->WriteBody(body);
  }
  LOG(LDEBUG, ("Connecting to", m_url, "[", m_begRange, ",", m_endRange, "]", "size=", m_expectedSize));
  r = m_pTransaction->Submit();
}

HttpThread::~HttpThread()
{
  m_pTransaction->RemoveHttpTransactionListener(*this);
  m_pSession->CloseTransaction(*m_pTransaction);
  delete m_pTransaction;
  delete m_pSession;
}


void 	HttpThread::OnTransactionHeaderCompleted (HttpSession &httpSession,
                                                HttpTransaction &httpTransaction,
                                                int headerLen, bool bAuthRequired)
{
  result r = E_SUCCESS;
  HttpResponse* pResponse = httpTransaction.GetResponse();
  if ((r = GetLastResult()) != E_SUCCESS)
  {
    LOG(LWARNING, ("httpTransaction.GetResponse error", r));
    return;
  }

  int const httpStatusCode = pResponse->GetHttpStatusCode();
  // When we didn't ask for chunks, code should be 200
  // When we asked for a chunk, code should be 206
  bool const isChunk = !(m_begRange == 0 && m_endRange < 0);
  if ((isChunk && httpStatusCode != 206) || (!isChunk && httpStatusCode != 200))
  {
    LOG(LWARNING, ("Http request to", m_url, " aborted with HTTP code", httpStatusCode));
    r = httpSession.CancelTransaction(httpTransaction);
    r = httpSession.CloseTransaction(httpTransaction);
    LOG(LDEBUG, ("CloseTransaction result", r));
    return;
  }
  else if (m_expectedSize > 0)
  {
    bool bGoodSize = false;
    // try to get content length from Content-Range header first
    HttpHeader * pHeader = pResponse->GetHeader();
    LOG(LDEBUG, ("Header:", FromTizenString(*pHeader->GetRawHeaderN())));
    IEnumerator * pValues = pHeader->GetFieldValuesN("Content-Range");
    if (GetLastResult() == E_SUCCESS)
    {
      bGoodSize = true;
      pValues->MoveNext(); // strange, but works
      String const * pString = dynamic_cast<String const *>(pValues->GetCurrent());

      if (pString->GetLength())
      {
        int lastInd;
        pString->LastIndexOf ('/', pString->GetLength()-1, lastInd);
        long long value = -1;
        String tail;
        pString->SubString(lastInd + 1, tail);
        LOG(LDEBUG, ("tail value:",FromTizenString(tail)));
        LongLong::Parse(tail, value);
        if (value != m_expectedSize)
        {
          LOG(LWARNING, ("Http request to", m_url,
                         "aborted - invalid Content-Range:", value, " expected:" ,m_expectedSize ));
          r = httpSession.CancelTransaction(httpTransaction);
          r = httpSession.CloseTransaction(httpTransaction);
          LOG(LDEBUG, ("CloseTransaction result", r));
        }
      }
    }
    else
    {

      pValues = pHeader->GetFieldValuesN("Content-Length");
      if (GetLastResult() == E_SUCCESS)
      {
        bGoodSize = true;
        pValues->MoveNext();  // strange, but works
        String const * pString = dynamic_cast<String const *>(pValues->GetCurrent());

        if (pString)
        {
          long long value = -1;
          LongLong::Parse(*pString, value);
          if (value != m_expectedSize)
          {
            LOG(LWARNING, ("Http request to", m_url,
                           "aborted - invalid Content-Length:", value, " expected:" ,m_expectedSize ));
            r = httpSession.CancelTransaction(httpTransaction);
            r = httpSession.CloseTransaction(httpTransaction);
            LOG(LDEBUG, ("CloseTransaction result", r));
          }
        }
      }
    }

    if (!bGoodSize)
    {

      LOG(LWARNING, ("Http request to", m_url,
                     "aborted, server didn't send any valid file size"));
      r = httpSession.CancelTransaction(httpTransaction);
      r = httpSession.CloseTransaction(httpTransaction);
      LOG(LDEBUG, ("CloseTransaction result", r));
    }
  }
}

void 	HttpThread::OnHttpDownloadInProgress (HttpSession &/*httpSession*/,
                                            Tizen::Net::Http::HttpTransaction &httpTransaction,
                                            long long currentLength,
                                            long long totalLength)
{
  HttpResponse* pResponse = httpTransaction.GetResponse();

  if (pResponse->GetHttpStatusCode() == HTTP_STATUS_OK
      || pResponse->GetHttpStatusCode() == HTTP_STATUS_PARTIAL_CONTENT)
  {
    ByteBuffer* pBuffer = null;
    pBuffer = pResponse->ReadBodyN();
    int chunkSize = pBuffer->GetLimit();
    m_downloadedBytes += chunkSize;
    m_callback.OnWrite(m_begRange + m_downloadedBytes - chunkSize, pBuffer->GetPointer(), chunkSize);
    delete pBuffer;
  }
  else
    LOG(LERROR, ("OnHttpDownloadInProgress ERROR", FromTizenString(pResponse->GetStatusText())));
}

void 	HttpThread::OnTransactionCompleted (HttpSession &/*httpSession*/,
                                          HttpTransaction & httpTransaction)
{
  HttpResponse* pResponse = httpTransaction.GetResponse();
  if (pResponse->GetHttpStatusCode() == HTTP_STATUS_OK
      || pResponse->GetHttpStatusCode() == HTTP_STATUS_PARTIAL_CONTENT)
  {
    m_callback.OnFinish(200, m_begRange, m_endRange);
  }
  else
  {
    LOG(LWARNING, ("Download has finished with status code:", pResponse->GetHttpStatusCode(),
                   " and text:", FromTizenString(pResponse->GetStatusText() )));
    m_callback.OnFinish(-100, m_begRange, m_endRange);
  }
}

void 	HttpThread::OnTransactionAborted (HttpSession &/*httpSession*/,
                                        HttpTransaction &/*httpTransaction*/,
                                        result r)
{
  LOG(LINFO, ("OnTransactionAborted result:", r));
  m_callback.OnFinish(-100, m_begRange, m_endRange);
}

void 	HttpThread::OnTransactionCertVerificationRequiredN (HttpSession &/*httpSession*/,
                                                          HttpTransaction &/*httpTransaction*/,
                                                          String */*pCert*/)
{
  LOG(LERROR, ("OnTransactionCertVerificationRequiredN"));
}

void 	HttpThread::OnTransactionReadyToRead (HttpSession &/*httpSession*/,
                                            HttpTransaction &/*httpTransaction*/,
                                            int /*availableBodyLen*/)
{
}

void 	HttpThread::OnTransactionReadyToWrite (HttpSession &/*httpSession*/,
                                             HttpTransaction &/*httpTransaction*/,
                                             int /*recommendedChunkSize*/)
{
}

void 	HttpThread::OnHttpUploadInProgress (Tizen::Net::Http::HttpSession &/*httpSession*/,
                                          Tizen::Net::Http::HttpTransaction &/*httpTransaction*/,
                                          long long /*currentLength*/,
                                          long long /*totalLength*/)
{
}
