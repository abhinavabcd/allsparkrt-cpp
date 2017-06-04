-- phpMyAdmin SQL Dump
-- version 4.6.4deb1
-- https://www.phpmyadmin.net/
--
-- Host: localhost:3306
-- Generation Time: May 07, 2017 at 05:24 PM
-- Server version: 5.7.18-0ubuntu0.16.10.1
-- PHP Version: 7.0.15-0ubuntu0.16.10.4

SET SQL_MODE = "NO_AUTO_VALUE_ON_ZERO";
SET time_zone = "+00:00";


/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!40101 SET NAMES utf8mb4 */;

--
-- Database: `allspark`
--

-- --------------------------------------------------------

--
-- Table structure for table `nodes`
--

CREATE TABLE `nodes` (
  `node_id` varchar(64) NOT NULL,
  `addr` varchar(100) DEFAULT NULL,
  `addr_internal` varchar(100) DEFAULT NULL,
  `port` varchar(6) DEFAULT NULL,
  `ssl_enabled` tinyint(1) NOT NULL DEFAULT '0',
  `is_internal` tinyint(1) NOT NULL DEFAULT '0',
  `created_timestamp` bigint(20) NOT NULL DEFAULT '0',
  `last_ping_received` bigint(20) NOT NULL DEFAULT '0',
  `is_connected_to_node` varchar(96) DEFAULT NULL
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

-- --------------------------------------------------------

--
-- Table structure for table `node_push_keys`
--

CREATE TABLE `node_push_keys` (
  `node_id` varchar(64) NOT NULL,
  `type` int(11) NOT NULL DEFAULT '0',
  `push_key` text
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

-- --------------------------------------------------------

--
-- Table structure for table `node_seq`
--

CREATE TABLE `node_seq` (
  `node_id` varchar(96) NOT NULL,
  `seq` bigint(20) NOT NULL DEFAULT '0',
  `last_updated` bigint(20) NOT NULL DEFAULT '0'
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

-- --------------------------------------------------------

--
-- Table structure for table `sessions`
--

CREATE TABLE `sessions` (
  `session_id` varchar(23) NOT NULL,
  `node_id` varchar(23) NOT NULL,
  `session_type` int(11) NOT NULL,
  `session_game_master_node_id` varchar(23) DEFAULT NULL,
  `notify_only_last_few_users` int(11) NOT NULL DEFAULT '256',
  `who_can_add_session_nodes` int(11) NOT NULL DEFAULT '0',
  `created_timestamp` bigint(20) NOT NULL DEFAULT '0',
  `usage_type` int(11) NOT NULL DEFAULT '0',
  `allow_anonymous` tinyint(1) NOT NULL DEFAULT '0',
  `description` varchar(256) DEFAULT NULL
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

-- --------------------------------------------------------

--
-- Table structure for table `session_nodes`
--

CREATE TABLE `session_nodes` (
  `session_id_node_id` varchar(50) NOT NULL,
  `session_id` varchar(23) NOT NULL,
  `node_id` varchar(64) NOT NULL,
  `anonymous_node_id` varchar(23) DEFAULT NULL,
  `is_anonymous` tinyint(1) NOT NULL DEFAULT '0',
  `timestamp` bigint(20) NOT NULL,
  `created_timestamp` bigint(20) NOT NULL DEFAULT '0'
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

-- --------------------------------------------------------

--
-- Table structure for table `templates`
--

CREATE TABLE `templates` (
  `template_id` varchar(96) NOT NULL,
  `render_doc` text NOT NULL,
  `instances_count` int(11) NOT NULL DEFAULT '0',
  `node_id` varchar(96) NOT NULL,
  `template_name` varchar(256) DEFAULT NULL
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

-- --------------------------------------------------------

--
-- Table structure for table `template_instances`
--

CREATE TABLE `template_instances` (
  `template_instance_id` varchar(96) NOT NULL,
  `render_doc` text NOT NULL,
  `node_id` varchar(96) NOT NULL,
  `session_id` varchar(96) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

-- --------------------------------------------------------

--
-- Table structure for table `user_inbox`
--

CREATE TABLE `user_inbox` (
  `node_id_seq` varchar(96) NOT NULL,
  `from_id` varchar(64) NOT NULL,
  `payload` varbinary(4096) DEFAULT NULL,
  `message_type` int(11) NOT NULL,
  `timestamp` bigint(20) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=latin1;

--
-- Indexes for dumped tables
--

--
-- Indexes for table `nodes`
--
ALTER TABLE `nodes`
  ADD PRIMARY KEY (`node_id`),
  ADD UNIQUE KEY `addr_2` (`addr`,`port`),
  ADD KEY `addr` (`addr`),
  ADD KEY `port` (`port`),
  ADD KEY `is_connected_to_node` (`is_connected_to_node`);

--
-- Indexes for table `node_push_keys`
--
ALTER TABLE `node_push_keys`
  ADD PRIMARY KEY (`node_id`);

--
-- Indexes for table `node_seq`
--
ALTER TABLE `node_seq`
  ADD PRIMARY KEY (`node_id`);

--
-- Indexes for table `sessions`
--
ALTER TABLE `sessions`
  ADD PRIMARY KEY (`session_id`),
  ADD KEY `node_id` (`node_id`,`session_type`,`usage_type`);

--
-- Indexes for table `session_nodes`
--
ALTER TABLE `session_nodes`
  ADD PRIMARY KEY (`session_id_node_id`);

--
-- Indexes for table `templates`
--
ALTER TABLE `templates`
  ADD PRIMARY KEY (`template_id`),
  ADD KEY `node_id` (`node_id`);

--
-- Indexes for table `template_instances`
--
ALTER TABLE `template_instances`
  ADD PRIMARY KEY (`template_instance_id`),
  ADD KEY `node_id` (`node_id`),
  ADD KEY `session_id` (`session_id`);

--
-- Indexes for table `user_inbox`
--
ALTER TABLE `user_inbox`
  ADD KEY `node_id_seq` (`node_id_seq`);

/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
