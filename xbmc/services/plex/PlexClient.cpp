/*
 *      Copyright (C) 2016 Team MrMC
 *      https://github.com/MrMC
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with MrMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <atomic>
#include <memory>
#include <algorithm>

#include "PlexClient.h"
#include "PlexUtils.h"
#include "PlexClientSync.h"

#include "Application.h"
#include "URL.h"
#include "filesystem/CurlFile.h"
#include "filesystem/StackDirectory.h"
#include "network/Network.h"
#include "settings/Settings.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/Base64URL.h"
#include "video/VideoInfoTag.h"

#include <string>
#include <sstream>

CPlexClient::CPlexClient()
{
  m_local = true;
  m_owned = "1";
  m_presence = true;
  m_protocol = "http";
  m_needUpdate = false;
  m_clientSync = nullptr;
}

CPlexClient::~CPlexClient()
{
}

bool CPlexClient::Init(std::string data, std::string ip)
{
  m_url = "";

  int port = 32400;
  m_protocol = "http";

  std::string s;
  std::istringstream f(data);
  while (std::getline(f, s))
  {
    int pos = s.find(':');
    if (pos > 0)
    {
      std::string substr = s.substr(0, pos);
      std::string name = StringUtils::Trim(substr);
      substr = s.substr(pos + 1);
      std::string val = StringUtils::Trim(substr);
      if (name == "Content-Type")
        m_contentType = val;
      else if (name == "Resource-Identifier")
        m_uuid = val;
      else if (name == "Name")
        m_serverName = val;
      else if (name == "Port")
        port = atoi(val.c_str());
    }
  }

  CURL url;
  url.SetHostName(ip);
  url.SetPort(port);
  url.SetProtocol(m_protocol);
  if (CPlexUtils::GetIdentity(url, 2))
    m_url = url.Get();

  return !m_url.empty();
}

bool CPlexClient::Init(const PlexServerInfo &serverInfo)
{
  m_url = "";
  m_presence = serverInfo.presence == "1";
  //if (!m_presence)
  //  return false;

  m_uuid = serverInfo.uuid;
  m_owned = serverInfo.owned;
  m_serverName = serverInfo.serverName;
  m_accessToken = serverInfo.accessToken;
  m_httpsRequired = serverInfo.httpsRequired;
  m_platform = serverInfo.platform;

  CURL url;
  if (!serverInfo.connections.empty())
  {
    for (const auto &connection : serverInfo.connections)
    {
      url.SetHostName(connection.address);
      url.SetPort(atoi(connection.port.c_str()));
      url.SetProtocol(connection.protocol);
      url.SetProtocolOptions("&X-Plex-Token=" + m_accessToken);
      int timeout = connection.external ? 5 : 1;
      if (CPlexUtils::GetIdentity(url, timeout))
      {
        CLog::Log(LOGDEBUG, "CPlexClient::Init "
          "serverName(%s), ipAddress(%s), protocol(%s)",
          m_serverName.c_str(), connection.address.c_str(), connection.protocol.c_str());

        m_url = url.Get();
        m_protocol = url.GetProtocol();
        m_local = (connection.external == 0);
        break;
      }
    }
  }


  if (m_clientSync)
    SAFE_DELETE(m_clientSync);

  if (!m_url.empty())
  {
/*
    m_clientSync = new CPlexClientSync(IsOwned(), m_serverName, url.GetWithoutFilename(),
      CSettings::GetInstance().GetString(CSettings::SETTING_SERVICES_UUID).c_str(), m_accessToken);
    m_clientSync->Start();
*/
  }

  return !m_url.empty();
}

CFileItemPtr CPlexClient::FindViewItemByServiceId(const std::string &serviceId)
{
  //CSingleLock lock(m_viewItemsLock);
  for (const auto &item : m_section_items)
  {
    if (item->GetMediaServiceId() == serviceId)
    {
      CLog::Log(LOGDEBUG, "CPlexClient::FindViewItemByServiceId: \"%s\"", item->GetLabel().c_str());
      return item;
    }
  }
  CLog::Log(LOGERROR, "CPlexClient::FindViewItemByServiceId: failed to get details for item with id \"%s\"", serviceId.c_str());
  return nullptr;
}

std::string CPlexClient::GetUrl()
{
  return m_url;
}

std::string CPlexClient::GetHost()
{
  CURL url(m_url);
  return url.GetHostName();
}

int CPlexClient::GetPort()
{
  CURL url(m_url);
  return url.GetPort();
}

const PlexSectionsContentVector CPlexClient::GetTvContent() const
{
  CSingleLock lock(m_criticalTVShow);
  return m_showSectionsContents;
}

const PlexSectionsContentVector CPlexClient::GetMovieContent() const
{
  CSingleLock lock(m_criticalMovies);
  return m_movieSectionsContents;
}

const PlexSectionsContentVector CPlexClient::GetArtistContent() const
{
  CSingleLock lock(m_criticalArtist);
  return m_artistSectionsContents;
}

const PlexSectionsContentVector CPlexClient::GetPhotoContent() const
{
  CSingleLock lock(m_criticalPhoto);
  return m_photoSectionsContents;
}

const std::string CPlexClient::FormatContentTitle(const std::string contentTitle) const
{
  std::string owned = (GetOwned() == "1") ? "O":"S";
  std::string title = StringUtils::Format("Plex(%s) - %s - %s %s",
              owned.c_str(), GetServerName().c_str(), contentTitle.c_str(), GetPresence()? "":"(off-line)");
  return title;
}

bool CPlexClient::IsSameClientHostName(const CURL& url)
{
  CURL real_url(url);
  if (real_url.GetProtocol() == "plex")
    real_url = CURL(Base64URL::Decode(URIUtils::GetFileName(real_url)));

  if (URIUtils::IsStack(real_url.Get()))
    real_url = CURL(XFILE::CStackDirectory::GetFirstStackedFile(real_url.Get()));
  
  return GetHost() == real_url.GetHostName();
}

bool CPlexClient::ParseSections(enum PlexSectionParsing parser)
{
  bool rtn = false;
  XFILE::CCurlFile plex;
  //plex.SetBufferSize(32768*10);
  plex.SetTimeout(10);

  CURL curl(m_url);
  curl.SetFileName(curl.GetFileName() + "library/sections");
  std::string strResponse;
  if (plex.Get(curl.Get(), strResponse))
  {
#if defined(PLEX_DEBUG_VERBOSE)
    if (parser == PlexSectionParsing::newSection)
      CLog::Log(LOGDEBUG, "CPlexClient::ParseSections %d, %s", parser, strResponse.c_str());
#endif
    if (parser == PlexSectionParsing::updateSection)
    {
      {
        CSingleLock lock(m_criticalMovies);
        m_movieSectionsContents.clear();
      }
      {
        CSingleLock lock(m_criticalTVShow);
        m_showSectionsContents.clear();
      }
      m_needUpdate = false;
    }

    TiXmlDocument xml;
    xml.Parse(strResponse.c_str());

    TiXmlElement* MediaContainer = xml.RootElement();
    if (MediaContainer)
    {
      const TiXmlElement* DirectoryNode = MediaContainer->FirstChildElement("Directory");
      while (DirectoryNode)
      {
        PlexSectionsContent content;
        content.uuid = XMLUtils::GetAttribute(DirectoryNode, "uuid");
        //content.path = XMLUtils::GetAttribute(DirectoryNode, "path");
        content.type = XMLUtils::GetAttribute(DirectoryNode, "type");
        content.title = XMLUtils::GetAttribute(DirectoryNode, "title");
        content.updatedAt = XMLUtils::GetAttribute(DirectoryNode, "updatedAt");
        std::string key = XMLUtils::GetAttribute(DirectoryNode, "key");
        content.section = "library/sections/" + key;
        content.thumb = XMLUtils::GetAttribute(DirectoryNode, "composite");
        std::string art = XMLUtils::GetAttribute(DirectoryNode, "art");
        if (m_local)
          content.art = art;
        else
          content.art = content.section + "/resources/" + URIUtils::GetFileName(art);
        if (content.type == "movie")
        {
          if (parser == PlexSectionParsing::checkSection)
          {
            CSingleLock lock(m_criticalMovies);
            for (const auto &contents : m_movieSectionsContents)
            {
              if (contents.uuid == content.uuid)
              {
                if (contents.updatedAt != content.updatedAt)
                {
#if defined(PLEX_DEBUG_VERBOSE)
                  CLog::Log(LOGDEBUG, "CPlexClient::ParseSections need update on %s:%s",
                    m_serverName.c_str(), content.title.c_str());
#endif
                  m_needUpdate = true;
                }
              }
            }
          }
          else
          {
            CSingleLock lock(m_criticalMovies);
            m_movieSectionsContents.push_back(content);
          }
        }
        else if (content.type == "show")
        {
          if (parser == PlexSectionParsing::checkSection)
          {
            CSingleLock lock(m_criticalTVShow);
            for (const auto &contents : m_showSectionsContents)
            {
              if (contents.uuid == content.uuid)
              {
                if (contents.updatedAt != content.updatedAt)
                {
#if defined(PLEX_DEBUG_VERBOSE)
                  CLog::Log(LOGDEBUG, "CPlexClient::ParseSections need update on %s:%s",
                    m_serverName.c_str(), content.title.c_str());
#endif
                  m_needUpdate = true;
                }
              }
            }
          }
          else
          {
            CSingleLock lock(m_criticalTVShow);
            m_showSectionsContents.push_back(content);
          }
        }
        else if (content.type == "artist")
        {
          if (parser == PlexSectionParsing::checkSection)
          {
            CSingleLock lock(m_criticalArtist);
            for (const auto &contents : m_artistSectionsContents)
            {
              if (contents.uuid == content.uuid)
              {
                if (contents.updatedAt != content.updatedAt)
                {
#if defined(PLEX_DEBUG_VERBOSE)
                  CLog::Log(LOGDEBUG, "CPlexClient::ParseSections need update on %s:%s",
                            m_serverName.c_str(), content.title.c_str());
#endif
                  m_needUpdate = true;
                }
              }
            }
          }
          else
          {
            CSingleLock lock(m_criticalArtist);
            m_artistSectionsContents.push_back(content);
          }
        }
        else if (content.type == "photo")
        {
          if (parser == PlexSectionParsing::checkSection)
          {
            CSingleLock lock(m_criticalPhoto);
            for (const auto &contents : m_photoSectionsContents)
            {
              if (contents.uuid == content.uuid)
              {
                if (contents.updatedAt != content.updatedAt)
                {
#if defined(PLEX_DEBUG_VERBOSE)
                  CLog::Log(LOGDEBUG, "CPlexClient::ParseSections need update on %s:%s",
                            m_serverName.c_str(), content.title.c_str());
#endif
                  m_needUpdate = true;
                }
              }
            }
          }
          else
          {
            CSingleLock lock(m_criticalPhoto);
            m_photoSectionsContents.push_back(content);
          }
        }
        else
        {
          CLog::Log(LOGDEBUG, "CPlexClient::ParseSections %s found unhandled content type %s",
            m_serverName.c_str(), content.type.c_str());
        }
        DirectoryNode = DirectoryNode->NextSiblingElement("Directory");
      }

      if (parser == PlexSectionParsing::newSection)
      {
        CLog::Log(LOGDEBUG, "CPlexClient::ParseSections %s found %d movie sections",
          m_serverName.c_str(), (int)m_movieSectionsContents.size());
        CLog::Log(LOGDEBUG, "CPlexClient::ParseSections %s found %d shows sections",
          m_serverName.c_str(), (int)m_showSectionsContents.size());
        CLog::Log(LOGDEBUG, "CPlexClient::ParseSections %s found %d artist sections",
                  m_serverName.c_str(), (int)m_artistSectionsContents.size());
        CLog::Log(LOGDEBUG, "CPlexClient::ParseSections %s found %d photo sections",
                  m_serverName.c_str(), (int)m_photoSectionsContents.size());
      }

      rtn = true;
    }
    else
    {
      CLog::Log(LOGDEBUG, "CPlexClient::ParseSections no MediaContainer found");
    }
  }
  else
  {
    // 401's are attempts to access a local server that is also in PMS
    // and these require an access token. Only local servers that are
    // not is PMS can be accessed via GDM.
    if (plex.GetResponseCode() != 401)
      CLog::Log(LOGDEBUG, "CPlexClient::ParseSections failed %s", strResponse.c_str());
    rtn = false;
  }

  return rtn;
}

void CPlexClient::SetPresence(bool presence)
{
  if (m_presence != presence)
  {
    m_presence = presence;
  }
}
