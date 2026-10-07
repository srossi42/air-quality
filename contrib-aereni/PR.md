# Accept Plantower PMS sensors in /ingest

## Contexte

L'API `/ingest` ne lit aujourd'hui les particules que sous les noms du SDS011
(`SDS_P1`, `SDS_P2`). Une station équipée d'un Plantower PMS5003 (ou PMS1003 à
PMS7003), que le firmware airRohr envoie sous les noms `PMS_P1` et `PMS_P2`,
est bien acceptée, mais ses mesures de PM10 et PM2.5 sont silencieusement
ignorées : la ligne est enregistrée avec des particules vides.

## Changement

- `ingest.py` : les noms de particules acceptés sont regroupés dans deux
  tuples, `PM10_VALUE_TYPES` et `PM25_VALUE_TYPES`, sur le modèle de ce qui
  existe déjà pour la température et l'humidité. `PMS_P1` et `PMS_P2` y
  rejoignent `SDS_P1` et `SDS_P2`. Ajouter un autre capteur plus tard se fait
  en une ligne.
- `docs/hardware.md` : le PMS5003 est mentionné comme alternative au SDS011.

Aucun changement de schéma de base ni de format d'API : les stations SDS011
existantes ne sont pas concernées.

## Hors périmètre

- `PMS_P0` (PM1), que les Plantower mesurent en plus, n'est pas stocké : la
  table `measurement` n'a pas de colonne pour lui. Je peux le proposer dans une
  PR séparée si ça vous intéresse.

## Vérification

Charge utile de test, au format airRohr :

```json
{
  "esp8266id": "a1b2c3d4e5f6",
  "software_version": "sonde-ext-1.1",
  "sensordatavalues": [
    { "value_type": "PMS_P1", "value": "14.7" },
    { "value_type": "PMS_P2", "value": "14.0" },
    { "value_type": "BME280_temperature", "value": "26.5" },
    { "value_type": "BME280_humidity", "value": "60.0" },
    { "value_type": "BME280_pressure", "value": "101380" }
  ]
}
```

Avant : `pm10` et `pm25` vides en base. Après : `14.7` et `14.0`.
