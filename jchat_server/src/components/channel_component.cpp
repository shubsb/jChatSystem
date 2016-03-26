/*
*   This file is part of the jChatSystem project.
*
*   This program is licensed under the GNU General
*   Public License. To view the full license, check
*   LICENSE in the project root.
*/

#include "components/channel_component.h"
#include "components/user_component.h"
#include "chat_server.h"
#include "protocol/version.h"
#include "protocol/components/channel_message_type.h"
#include "string.hpp"

namespace jchat {
ChannelComponent::ChannelComponent() {
}

ChannelComponent::~ChannelComponent() {
  if (!channels_.empty()) {
    channels_.clear();
  }
}

bool ChannelComponent::Initialize(ChatServer &server) {
  server_ = &server;
  return true;
}

bool ChannelComponent::Shutdown() {
  server_ = 0;

  // Remove channels
  channels_mutex_.lock();
  if (!channels_.empty()) {
    channels_.clear();
  }
  channels_mutex_.unlock();

  return true;
}

bool ChannelComponent::OnStart() {
  return true;
}

bool ChannelComponent::OnStop() {
  // Remove channels
  channels_mutex_.lock();
  if (!channels_.empty()) {
    channels_.clear();
  }
  channels_mutex_.unlock();

  return true;
}

void ChannelComponent::OnClientConnected(RemoteChatClient &client) {

}

void ChannelComponent::OnClientDisconnected(RemoteChatClient &client) {
  // Notify all clients in participating channels that the client has
  // disconnected
  channels_mutex_.lock();
  for (auto it = channels_.begin(); it != channels_.end(); ++it) {
    std::shared_ptr<ChatChannel> channel = *it;

    if (channel->Enabled) {
      channel->ClientsMutex.lock();
      if (channel->Clients.find(&client) != channel->Clients.end()) {
        // Get the chat user
        std::shared_ptr<ChatUser> chat_user = channel->Clients[&client];

        // Notify all clients in that channel that the client left
        TypedBuffer clients_buffer = server_->CreateBuffer();
        clients_buffer.WriteUInt16(kChannelMessageResult_UserLeft);
        clients_buffer.WriteString(chat_user->Username);
        clients_buffer.WriteString(chat_user->Hostname);

        for (auto &pair : channel->Clients) {
          if (pair.second->Enabled) {
            server_->SendUnicast(pair.first, kComponentType_Channel,
              kChannelMessageType_LeaveChannel, clients_buffer);
          }
        }

        // Trigger the events
        OnChannelLeft(*channel, *chat_user);

        // Remove the client from the clients list
        channel->Clients.erase(&client);

        // If there was nobody in the channel delete it
        if (channel->Clients.empty()) {
          channel->Enabled = false;
          channel.reset();
        }
      }
      channel->ClientsMutex.unlock();

      // Remove the client from the operators list if they're an operator
      channel->OperatorsMutex.lock();
      if (channel->Operators.find(&client) != channel->Clients.end()) {
        channel->Operators.erase(&client);
      }
      channel->OperatorsMutex.unlock();
    }
  }
  channels_mutex_.unlock();
}

ComponentType ChannelComponent::GetType() {
  return kComponentType_Channel;
}

bool ChannelComponent::Handle(RemoteChatClient &client, uint16_t message_type,
  TypedBuffer &buffer) {
  if (message_type == kChannelMessageType_JoinChannel) {
    std::string channel_name;
    if (!buffer.ReadString(channel_name)) {
      return false;
    }

    // Get user component
    UserComponent *user_component = 0;
    if (!server_->GetComponent(kComponentType_User, user_component)) {
      // Internal error, disconnect client
      return false;
    }

    // Get the chat client
    std::shared_ptr<ChatUser> chat_user;
    if (!user_component->GetChatUser(client, chat_user)) {
      // Internal error, disconnect client
      return false;
    }

    // Check if the user is logged in
    if (!chat_user->Identified) {
      TypedBuffer send_buffer = server_->CreateBuffer();
      send_buffer.WriteUInt16(kChannelMessageResult_NotIdentified);
      server_->SendUnicast(client, kComponentType_Channel,
        kChannelMessageType_JoinChannel_Complete, send_buffer);

      OnJoinCompleted(kChannelMessageResult_NotIdentified, *chat_user);

      return true;
    }

    // Check if the channel name is valid
    if (!String::Contains(channel_name, "#")) {
      TypedBuffer send_buffer = server_->CreateBuffer();
      send_buffer.WriteUInt16(kChannelMessageResult_InvalidChannelName);
      server_->SendUnicast(client, kComponentType_Channel,
        kChannelMessageType_JoinChannel_Complete, send_buffer);

      OnJoinCompleted(kChannelMessageResult_InvalidChannelName, *chat_user);

      return true;
    }

    // Check if the channel exists
    std::shared_ptr<ChatChannel> chat_channel;
    channels_mutex_.lock();
    for (auto &channel : channels_) {
      if (channel->Enabled && channel->Name == channel_name) {
        chat_channel = channel;
        break;
      }
    }
    channels_mutex_.unlock();

    if (!chat_channel) {
      // Create the channel and add the user to it
      auto chat_channel = std::make_shared<ChatChannel>();
      chat_channel->Name = channel_name;
      chat_channel->Operators[&client] = chat_user;
      chat_channel->Clients[&client] = chat_user;

      // Add the channel to the component
      channels_mutex_.lock();
      channels_.push_back(chat_channel);
      channels_mutex_.unlock();

      // Notify the client that the channel was created and that they are
      // the operator operator and member of it
      TypedBuffer send_buffer = server_->CreateBuffer();
      send_buffer.WriteUInt16(kChannelMessageResult_ChannelCreated);
      server_->SendUnicast(client, kComponentType_Channel,
        kChannelMessageType_JoinChannel_Complete, send_buffer);

      OnJoinCompleted(kChannelMessageResult_ChannelCreated, *chat_user);

      // Trigger the events
      OnChannelCreated(*chat_channel);
      OnChannelJoined(*chat_channel, *chat_user);

      return true;
    }

    // Check if the user is already in the channel
    chat_channel->ClientsMutex.lock();
    if (chat_channel->Clients.find(&client) != chat_channel->Clients.end()) {
      chat_channel->ClientsMutex.unlock();

      TypedBuffer send_buffer = server_->CreateBuffer();
      send_buffer.WriteUInt16(kChannelMessageResult_AlreadyInChannel);
      server_->SendUnicast(client, kComponentType_Channel,
        kChannelMessageType_JoinChannel_Complete, send_buffer);

      OnJoinCompleted(kChannelMessageResult_AlreadyInChannel, *chat_user);

      return true;
    }

    // Add the user to the channel
    chat_channel->Clients[&client] = chat_user;
    chat_channel->ClientsMutex.unlock();

    // Notify the client that it joined the channel and give it a list of
    // current clients
    TypedBuffer client_buffer = server_->CreateBuffer();
    client_buffer.WriteUInt16(kChannelMessageResult_Ok); // Channel joined

    chat_channel->OperatorsMutex.lock();
    client_buffer.WriteUInt32(chat_channel->Operators.size());
    for (auto &pair : chat_channel->Operators) {
      if (pair.second->Enabled) {
        client_buffer.WriteString(pair.second->Username);
        client_buffer.WriteString(pair.second->Hostname);
      }
    }
    chat_channel->OperatorsMutex.unlock();

    chat_channel->ClientsMutex.lock();
    client_buffer.WriteUInt32(chat_channel->Clients.size());
    for (auto &pair : chat_channel->Clients) {
      if (pair.second->Enabled) {
        client_buffer.WriteString(pair.second->Username);
        client_buffer.WriteString(pair.second->Hostname);
      }
    }
    chat_channel->ClientsMutex.unlock();
    server_->SendUnicast(client, kComponentType_Channel,
      kChannelMessageType_JoinChannel_Complete, client_buffer);

    // Notify all clients in the channel that the user has joined
    TypedBuffer clients_buffer = server_->CreateBuffer();
    clients_buffer.WriteUInt16(kChannelMessageResult_UserJoined);
    clients_buffer.WriteString(chat_user->Username);
    clients_buffer.WriteString(chat_user->Hostname);

    chat_channel->ClientsMutex.lock();
    for (auto &pair : chat_channel->Clients) {
      if (pair.second->Enabled) {
        server_->SendUnicast(pair.first, kComponentType_Channel,
          kChannelMessageType_JoinChannel, clients_buffer);
      }
    }
    chat_channel->ClientsMutex.unlock();

    // Trigger the events
    OnJoinCompleted(kChannelMessageResult_Ok, *chat_user);
    OnChannelJoined(*chat_channel, *chat_user);

    return true;
  } else if (message_type == kChannelMessageType_LeaveChannel) {
    std::string channel_name;
    if (!buffer.ReadString(channel_name)) {
      return false;
    }

    // Get user component
    UserComponent *user_component = 0;
    if (!server_->GetComponent(kComponentType_User, user_component)) {
      // Internal error, disconnect client
      return false;
    }

    // Get the chat client
    std::shared_ptr<ChatUser> chat_user;
    if (!user_component->GetChatUser(client, chat_user)) {
      // Internal error, disconnect client
      return false;
    }

    // Check if the user is logged in
    if (!chat_user->Identified) {
      TypedBuffer send_buffer = server_->CreateBuffer();
      send_buffer.WriteUInt16(kChannelMessageResult_NotIdentified);
      server_->SendUnicast(client, kComponentType_Channel,
        kChannelMessageType_LeaveChannel_Complete, send_buffer);

      OnLeaveCompleted(kChannelMessageResult_NotIdentified, *chat_user);

      return true;
    }

    // Check if the channel name is valid
    if (!String::Contains(channel_name, "#")) {
      TypedBuffer send_buffer = server_->CreateBuffer();
      send_buffer.WriteUInt16(kChannelMessageResult_InvalidChannelName);
      server_->SendUnicast(client, kComponentType_Channel,
        kChannelMessageType_LeaveChannel_Complete, send_buffer);

      OnLeaveCompleted(kChannelMessageResult_InvalidChannelName, *chat_user);

      return true;
    }

    // Check if the channel exists
    std::shared_ptr<ChatChannel> chat_channel;
    channels_mutex_.lock();
    for (auto &channel : channels_) {
      if (channel->Enabled && channel->Name == channel_name) {
        chat_channel = channel;
        break;
      }
    }
    channels_mutex_.unlock();

    if (!chat_channel) {
      TypedBuffer send_buffer = server_->CreateBuffer();
      send_buffer.WriteUInt16(kChannelMessageResult_InvalidChannelName);
      server_->SendUnicast(client, kComponentType_Channel,
        kChannelMessageType_LeaveChannel_Complete, send_buffer);

      OnLeaveCompleted(kChannelMessageResult_InvalidChannelName, *chat_user);

      return true;
    }

    // Check if the user is in the channel
    chat_channel->ClientsMutex.lock();
    if (chat_channel->Clients.find(&client) != chat_channel->Clients.end()) {
      // Notify all clients in that channel that the client left
      TypedBuffer clients_buffer = server_->CreateBuffer();
      clients_buffer.WriteUInt16(kChannelMessageResult_UserLeft);
      clients_buffer.WriteString(chat_user->Username);
      clients_buffer.WriteString(chat_user->Hostname);

      for (auto &pair : chat_channel->Clients) {
        if (pair.second->Enabled) {
          server_->SendUnicast(pair.first, kComponentType_Channel,
            kChannelMessageType_LeaveChannel, clients_buffer);
        }
      }

      // Trigger the events
      OnChannelLeft(*chat_channel, *chat_user);

      // Remove the client from the clients list
      chat_channel->Clients.erase(&client);

      // If there was nobody in the channel delete it
      if (chat_channel->Clients.empty()) {
        chat_channel->Enabled = false;
        chat_channel.reset();
      }
    } else {
      chat_channel->ClientsMutex.unlock();

      // Notify the client that they are not in the channel
      TypedBuffer send_buffer = server_->CreateBuffer();
      send_buffer.WriteUInt16(kChannelMessageResult_NotInChannel);
      server_->SendUnicast(client, kComponentType_Channel,
        kChannelMessageType_LeaveChannel_Complete, send_buffer);

      OnLeaveCompleted(kChannelMessageResult_NotInChannel, *chat_user);

      return true;
    }
    chat_channel->ClientsMutex.unlock();

    // Remove the client from the operators list if they're an operator
    chat_channel->OperatorsMutex.lock();
    if (chat_channel->Operators.find(&client) != chat_channel->Clients.end()) {
      chat_channel->Operators.erase(&client);
    }
    chat_channel->OperatorsMutex.unlock();

    // Notify the client that they left the channel
    TypedBuffer send_buffer = server_->CreateBuffer();
    send_buffer.WriteUInt16(kChannelMessageResult_Ok);
    send_buffer.WriteString(channel_name);
    server_->SendUnicast(client, kComponentType_Channel,
      kChannelMessageType_LeaveChannel_Complete, send_buffer);

    OnLeaveCompleted(kChannelMessageResult_Ok, *chat_user);

    return true;
  }

  return false;
}
}
