<?php

use Monolog\Handler\BrowserConsoleHandler;
use Monolog\Handler\ErrorLogHandler;
use Monolog\Logger;
use Symfony\Component\Config\FileLocator;
use Symfony\Component\DependencyInjection\ContainerBuilder;
use Symfony\Component\DependencyInjection\Loader\XmlFileLoader;

$container = new ContainerBuilder();

$container->setParameter('application_root', dirname(dirname(__DIR__)));

$loader = new XmlFileLoader($container, new FileLocator(__DIR__));
$loader->load('services.xml');

$container->compile();

$GLOBALS['container'] = $container;

$logger = $container->get('logger');
$logger->pushHandler(new ErrorLogHandler(ErrorLogHandler::OPERATING_SYSTEM, Logger::INFO));
$logger->pushHandler(new BrowserConsoleHandler(Logger::DEBUG));
